// book_replay: build L2 books for every symbol over a full ITCH file, check the
// feed invariants, audit the books against the live orders, and compare each
// symbol's official closing-cross price with its book at the 16:00 bell.
//
//   book_replay <file> [--levels vector|map] [--order-map-log2 N] [--repeat N]
//               [--show SYM,...]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "book/book_builder.hpp"
#include "itch/mapped_file.hpp"
#include "itch/parser.hpp"

namespace {

double px(std::uint32_t p) { return p / 1e4; }

constexpr std::uint64_t hms(int h, int m, int s) { return ((h * 60ULL + m) * 60 + s) * 1'000'000'000ULL; }

struct NullHandler {};  // parse-only: every message type is skipped at compile time

template <template <bool> class L>
int run(const itch::MappedFile& file, const std::vector<std::string>& show, unsigned map_log2, int repeat) {
    // Parse-only pass: if this is far above ~2 s, pages are coming from disk and
    // the book timings below are I/O-bound, not a measurement of the books.
    NullHandler null;
    const auto tp = std::chrono::steady_clock::now();
    const auto pr = itch::parse(file.data(), file.end(), null);
    std::printf("parse-only pass:   %.3f s   (%llu msgs)\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - tp).count(),
                (unsigned long long)pr.messages);

    std::unique_ptr<book::BookBuilder<L>> builder;
    itch::ParseResult r;
    std::vector<double> secs;
    for (int i = 0; i < repeat; ++i) {
        // Audit mid-session, when the books are full; at end of day they are empty.
        builder = std::make_unique<book::BookBuilder<L>>(
            map_log2, std::vector<std::uint64_t>{hms(9, 30, 1), hms(12, 0, 0), hms(15, 59, 59)});
        const auto t0 = std::chrono::steady_clock::now();
        r = itch::parse(file.data(), file.end(), *builder);
        secs.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() -
                       builder->audit_seconds());
    }
    std::sort(secs.begin(), secs.end());
    const double s = secs.front();

    std::printf("messages:          %llu (parse error %d)\n", (unsigned long long)r.messages, (int)r.error);
    std::printf("book passes:       %d   best %.3f s   median %.3f s   (audits excluded)\n", repeat, s,
                secs[secs.size() / 2]);
    std::printf("throughput (best): %.1f M msgs/s   %.1f ns/msg\n", r.messages / s / 1e6, s * 1e9 / r.messages);
    std::printf("peak live orders:  %zu\n", builder->peak_live_orders());
    std::printf("live at end:       %zu\n", builder->live_orders());
    std::printf("top-of-book hash:  %016llx\n", (unsigned long long)builder->tob_hash());

    const auto& v = builder->violations();
    std::printf("\ninvariants\n");
    std::printf("  unknown order ref        %llu\n", (unsigned long long)v.unknown_ref);
    std::printf("  exec/cancel > remaining  %llu\n", (unsigned long long)v.overfill);
    std::printf("  locate mismatch          %llu\n", (unsigned long long)v.locate_mismatch);
    std::printf("  duplicate add            %llu\n", (unsigned long long)v.duplicate_add);
    std::printf("  level mismatch           %llu\n", (unsigned long long)v.level_mismatch);
    std::printf("  crossed (open, trading)  %llu\n", (unsigned long long)v.crossed_open);
    std::printf("  locked  (open, trading)  %llu\n", (unsigned long long)v.locked_open);
    std::printf("  crossed (pre/post/halt)  %llu   (allowed)\n", (unsigned long long)v.crossed_other);

    std::uint64_t audit_mismatches = 0;
    for (const auto& a : builder->audits()) {
        audit_mismatches += a.mismatches;
        std::printf("  audit at %02llu:%02llu:%02llu  %8zu live orders  %llu level mismatches\n",
                    (unsigned long long)(a.timestamp / hms(1, 0, 0)), (unsigned long long)(a.timestamp / hms(0, 1, 0) % 60),
                    (unsigned long long)(a.timestamp / hms(0, 0, 1) % 60), a.live_orders,
                    (unsigned long long)a.mismatches);
    }
    if (builder->audits().size() != 3) {
        std::printf("  expected 3 audits, ran %zu\n", builder->audits().size());
        ++audit_mismatches;
    }

    // Closing cross vs. the midpoint of the displayed book at 16:00. Not an
    // invariant: the cross includes on-close orders that never display, so it
    // can legitimately print outside the continuous-market quote.
    const auto& crosses = builder->closing_crosses();
    const auto& bbo = builder->close_bbo();
    std::vector<double> bps;
    std::size_t inside = 0, traded = 0;
    for (const auto& c : crosses) {
        if (bbo.empty()) break;
        if (c.shares == 0) continue;  // every symbol gets a 'Q'; untraded ones carry price 0
        ++traded;
        const auto& q = bbo[c.locate];
        if (q.bid == 0 || q.ask == 0 || q.bid >= q.ask) continue;
        const double mid = (double(q.bid) + q.ask) / 2;
        bps.push_back(std::abs(c.price - mid) / mid * 1e4);
        inside += q.bid <= c.price && c.price <= q.ask;
    }
    std::sort(bps.begin(), bps.end());
    auto pct = [&](double p) { return bps.empty() ? 0.0 : bps[std::min(bps.size() - 1, std::size_t(p * bps.size()))]; };
    std::printf("\nclosing crosses:   %zu; %zu with volume; %zu of those with a two-sided 16:00 book, %.1f%% priced inside it\n",
                crosses.size(), traded, bps.size(), bps.empty() ? 0.0 : 100.0 * inside / bps.size());
    std::printf("  |cross - 16:00 mid| in bps: p50 %.1f  p90 %.1f  p99 %.1f\n", pct(0.5), pct(0.9), pct(0.99));

    for (const auto& sym : show) {
        const int loc = builder->locate_of(sym);
        if (loc < 0) {
            std::printf("  %-6s not in directory\n", sym.c_str());
            continue;
        }
        const book::Bbo q = bbo.empty() ? book::Bbo{} : bbo[loc];
        std::uint32_t close = 0;
        std::uint64_t close_sh = 0;
        for (const auto& c : crosses)
            if (c.locate == loc) close = c.price, close_sh = c.shares;
        std::printf("  %-6s 16:00 book %9.4f x %-9.4f  closing cross %9.4f  (%llu sh)\n", sym.c_str(), px(q.bid),
                    px(q.ask), px(close), (unsigned long long)close_sh);
    }

    return (r.error == itch::ParseError::none && v.hard() == 0 && audit_mismatches == 0) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <itch-file> [--levels vector|map] [--show SYM,SYM,...]\n", argv[0]);
        return 2;
    }
    std::string levels = "vector";
    unsigned map_log2 = 22;
    int repeat = 1;
    std::vector<std::string> show = {"QQQ", "AAPL", "MSFT", "AMZN", "GOOGL", "FB", "NVDA"};
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--levels") && i + 1 < argc) {
            levels = argv[++i];
        } else if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) {
            repeat = std::max(1, std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--order-map-log2") && i + 1 < argc) {
            map_log2 = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--show") && i + 1 < argc) {
            show.clear();
            std::stringstream ss(argv[++i]);
            for (std::string s; std::getline(ss, s, ',');) show.push_back(s);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    itch::MappedFile file(argv[1]);
    file.prefault();
    std::printf("levels policy:     %s, order map 2^%u slots\n", levels.c_str(), map_log2);
    if (levels == "vector") return run<book::VectorLevels>(file, show, map_log2, repeat);
    if (levels == "map") return run<book::MapLevels>(file, show, map_log2, repeat);
    std::fprintf(stderr, "unknown levels policy: %s\n", levels.c_str());
    return 2;
}
