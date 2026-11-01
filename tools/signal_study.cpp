// signal_study: replays an ITCH file through the order books with the signal
// engine attached, samples every watched symbol's signals at each top-of-book
// change during regular trading, and reports how well each signal predicts the
// next mid move and the mid return over a fixed horizon.
//
//   signal_study <file> [--symbols A,B,...] [--from HH:MM:SS] [--to HH:MM:SS]
//                [--horizon-ms N] [--tau-ms N]
//
// Samples are taken only while the symbol is trading ('T'), its book is
// two-sided, and the time is in [--from, --to). Defaults: 09:35:00-15:55:00,
// horizon 1000 ms, tau 1000 ms.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "book/book_builder.hpp"
#include "cli.hpp"
#include "itch/mapped_file.hpp"
#include "itch/parser.hpp"
#include "signals/signal_engine.hpp"
#include "signals/study.hpp"

namespace {

// First pass: symbol names by stock locate (the directory is at the start of the day).
struct Directory {
    std::vector<std::string> names = std::vector<std::string>(65536);
    void on(const itch::StockDirectory& m) { names[m.hdr.stock_locate] = std::string(itch::trim(m.stock)); }
};

struct StudyListener {
    signals::SignalEngine engine;
    std::vector<std::unique_ptr<signals::Labeler>> labelers = std::vector<std::unique_ptr<signals::Labeler>>(65536);
    std::uint64_t from = 0, to = 0;

    template <class Book>
    void on_book_update(std::uint16_t locate, std::uint64_t ts, const Book& b) {
        const book::Bbo q = b.bbo();
        if (!engine.update_bbo(locate, ts, q)) return;
        signals::Labeler* lab = labelers[locate].get();
        if (!lab) return;
        if (!signals::two_sided(q)) {
            lab->drop_at(ts);
            return;
        }
        lab->on_change(ts, signals::mid(q));
        if (ts < from || ts >= to || b.trading_state != 'T') return;
        const signals::Snapshot s = engine.snapshot(locate, ts);
        lab->add(ts, {s.imbalance, s.micro_bps, s.ofi, s.flow});
    }

    void on_execution(std::uint16_t locate, std::uint64_t ts, char side, std::uint32_t shares, std::uint32_t price) {
        engine.on_execution(locate, ts, side, shares, price);
    }
};

void print_report(const char* name, const signals::Report& r) {
    std::printf("  %-10s n=%-9zu hit %5.1f%% (of %zu)  corr %+.3f  deciles bps:", name, r.n, 100 * r.hit_rate,
                r.hits_n, r.corr);
    for (double d : r.decile_bps) std::printf(" %+6.2f", d);
    std::printf("\n");
}

}  // namespace

int run(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <itch-file> [--symbols A,B,...] [--from HH:MM:SS] [--to HH:MM:SS] "
                     "[--horizon-ms N] [--tau-ms N]\n",
                     argv[0]);
        return 2;
    }
    std::vector<std::string> symbols = {"AAPL", "MSFT", "AMZN", "GOOGL", "FB", "NVDA", "INTC", "CSCO", "QQQ", "TSLA"};
    std::uint64_t from = cli::parse_hms("09:35:00"), to = cli::parse_hms("15:55:00");
    double horizon_ms = 1000, tau_ms = 1000;
    for (int i = 2; i < argc; ++i) {
        auto opt = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
        if (opt("--symbols")) {
            symbols.clear();
            std::stringstream ss(argv[++i]);
            for (std::string s; std::getline(ss, s, ',');) symbols.push_back(s);
        } else if (opt("--from")) from = cli::parse_hms(argv[++i]);
        else if (opt("--to")) to = cli::parse_hms(argv[++i]);
        else if (opt("--horizon-ms")) horizon_ms = std::atof(argv[++i]);
        else if (opt("--tau-ms")) tau_ms = std::atof(argv[++i]);
        else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!(horizon_ms > 0) || !(tau_ms > 0) || to <= from) {
        std::fprintf(stderr, "need horizon-ms > 0, tau-ms > 0 and --to after --from\n");
        return 2;
    }

    itch::MappedFile file(argv[1]);
    file.prefault();
    Directory dir;
    itch::parse(file.data(), file.end(), dir);

    // Not aggregate-initialized: `{}` for labelers would replace its 65536-entry default with an empty vector.
    StudyListener listener;
    listener.engine = signals::SignalEngine(tau_ms * 1e6);
    listener.from = from;
    listener.to = to;
    std::vector<std::uint16_t> watched;
    for (const std::string& sym : symbols) {
        int loc = -1;
        for (std::size_t l = 0; l < dir.names.size(); ++l)
            if (dir.names[l] == sym) loc = static_cast<int>(l);
        if (loc < 0) {
            std::printf("  %-6s not in directory, skipped\n", sym.c_str());
            continue;
        }
        if (std::find(watched.begin(), watched.end(), static_cast<std::uint16_t>(loc)) != watched.end()) continue;
        watched.push_back(static_cast<std::uint16_t>(loc));
        listener.labelers[static_cast<std::size_t>(loc)] =
            std::make_unique<signals::Labeler>(static_cast<std::uint64_t>(horizon_ms * 1e6));
    }

    if (watched.empty()) {
        std::fprintf(stderr, "error: none of the requested symbols is in the directory\n");
        return 2;
    }

    auto books = std::make_unique<book::BookBuilder<book::VectorLevels, StudyListener>>(
        22, std::vector<std::uint64_t>{}, std::move(listener));
    const auto t0 = std::chrono::steady_clock::now();
    const itch::ParseResult pr = itch::parse(file.data(), file.end(), *books);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const StudyListener& L = books->listener();
    auto hms = [](std::uint64_t ns) {
        return std::array<unsigned long long, 3>{ns / 3'600'000'000'000ULL, ns / 60'000'000'000ULL % 60,
                                                 ns / 1'000'000'000ULL % 60};
    };
    const auto f = hms(from), t = hms(to);
    std::printf("signal study: %zu symbols, %02llu:%02llu:%02llu-%02llu:%02llu:%02llu, horizon %.0f ms, tau %.0f ms\n",
                watched.size(), f[0], f[1], f[2], t[0], t[1], t[2], horizon_ms, tau_ms);
    std::printf("replay: %llu messages in %.2f s (%.1f ns/msg, books + signals for all symbols)\n",
                (unsigned long long)pr.messages, secs, secs * 1e9 / double(pr.messages));
    std::printf("top-of-book hash: %016llx\n\n", (unsigned long long)books->tob_hash());

    std::printf("  %-6s %12s %12s %10s %10s\n", "symbol", "bbo changes", "executions", "samples", "imb hit");
    std::vector<const std::vector<signals::Sample>*> all;
    for (std::uint16_t loc : watched) {
        const auto& samples = L.labelers[loc]->samples();
        all.push_back(&samples);
        const signals::Report r = signals::evaluate({&samples}, 0);
        std::printf("  %-6s %12llu %12llu %10zu %9.1f%%\n", dir.names[loc].c_str(),
                    (unsigned long long)L.engine.bbo_changes(loc), (unsigned long long)L.engine.executions(loc), r.n,
                    100 * r.hit_rate);
    }

    std::printf("\npooled over symbols (deciles low -> high signal; mean %.0f ms forward mid return):\n", horizon_ms);
    for (std::size_t k = 0; k < signals::kNumSignals; ++k)
        print_report(signals::kSignalNames[k], signals::evaluate(all, k));

    const bool ok = pr.error == itch::ParseError::none && books->violations().hard() == 0;
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
