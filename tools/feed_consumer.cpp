// feed_consumer: joins a MoldUDP64 ITCH multicast feed, builds every symbol's
// book as packets arrive, and measures tick-to-book latency: from the kernel's
// receive timestamp on a datagram to the moment its last message has been
// applied to the books.
//
//   feed_consumer [--group A] [--port P] [--iface A] [--rcvbuf BYTES] [--cpu N]
//                 [--from HH:MM:SS] [--start-timeout SECONDS] [--idle-timeout SECONDS]
//                 [--signals] [--show SYM,...]
//
// Latency is reported in two parts plus the total, and the service time:
//   queue = recvmmsg() returned - kernel receive    (waiting in the socket buffer)
//   book  = books updated       - recvmmsg() returned (parse + book updates, including
//                                                      packets ahead of it in the batch)
//   svc   = books updated       - start of this packet's own work: the later of
//           recvmmsg() returned and the previous packet in the batch finishing
//           (parse + book updates for this packet alone, no head-of-line wait)
// Only packets whose first message is at or after --from are measured. Samples
// where the realtime clock visibly stepped (backwards, or total > 10 s) are
// counted as clock steps and left out. The forward cutoff is 10 s: a 256 MB
// receive buffer can legitimately hold a backlog of more than 1 s.
// --signals attaches the signal engine (all symbols), so its cost lands in the
// book and svc histograms; --show lists symbols whose final signals are printed
// (default AAPL,MSFT,QQQ) and implies --signals. Without --signals the listener
// still costs a null check per book update, an extra order lookup per execution
// and a timestamp store per message, so that mode is not byte-identical to the
// Stage 3 consumer.
// Exits 1 if no datagram arrives within --start-timeout of joining (default
// 30 s); after the first one, --idle-timeout (default 5 s) of silence ends the
// run. Exits nonzero on any gap, malformed or truncated packet, packet without a
// kernel timestamp, hard book invariant violation, or if no end-of-session
// packet arrives.

#include <algorithm>
#include <cmath>
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
#include "net/feed_handler.hpp"
#include "net/os.hpp"
#include "net/udp.hpp"
#include "signals/signal_engine.hpp"
#include "stats/histogram.hpp"

namespace {

void print_hist(const char* name, const stats::Histogram& h) {
    auto us = [](std::uint64_t ns) { return double(ns) / 1e3; };
    std::printf("  %-5s n=%-9llu min %7.2f  p50 %7.2f  p90 %7.2f  p99 %8.2f  p99.9 %8.2f  p99.99 %9.2f  max %9.2f  mean %7.2f\n",
                name, (unsigned long long)h.count(), us(h.min()), us(h.percentile(0.5)), us(h.percentile(0.9)),
                us(h.percentile(0.99)), us(h.percentile(0.999)), us(h.percentile(0.9999)), us(h.max()),
                h.mean() / 1e3);
}

// Book listener that forwards to the signal engine only when one is attached.
struct MaybeSignals {
    signals::SignalEngine* engine = nullptr;

    template <class B>
    void on_book_update(std::uint16_t locate, std::uint64_t ts, const B& b) {
        if (engine) engine->on_book_update(locate, ts, b);
    }
    void on_execution(std::uint16_t locate, std::uint64_t ts, char side, std::uint32_t shares, std::uint32_t price) {
        if (engine) engine->on_execution(locate, ts, side, shares, price);
    }
};

}  // namespace

int run(int argc, char** argv) {
    net::McastAddr addr;
    int rcvbuf = 256 << 20, cpu = -1;
    std::uint64_t from = 0;
    bool with_signals = false;
    std::vector<std::string> show;
    double idle_s = 5, start_s = 30;
    for (int i = 1; i < argc; ++i) {
        auto opt = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
        if (!std::strcmp(argv[i], "--signals")) {
            with_signals = true;
        } else if (opt("--show")) {
            std::stringstream ss(argv[++i]);
            for (std::string s; std::getline(ss, s, ',');) show.push_back(s);
        } else if (opt("--group")) addr.group = argv[++i];
        else if (opt("--port")) addr.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (opt("--iface")) addr.iface = argv[++i];
        else if (opt("--rcvbuf")) rcvbuf = std::atoi(argv[++i]);
        else if (opt("--cpu")) cpu = std::atoi(argv[++i]);
        else if (opt("--from")) from = cli::parse_hms(argv[++i]);
        else if (opt("--start-timeout")) start_s = std::atof(argv[++i]);
        else if (opt("--idle-timeout")) idle_s = std::atof(argv[++i]);
        else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!show.empty()) with_signals = true;
    if (cpu >= 0 && !net::pin_to_cpu(cpu)) std::perror("pin_to_cpu");

    int granted = 0;
    net::Socket sock = net::open_mcast_receiver(addr, rcvbuf, &granted);
    std::printf("joined %s:%u   rcvbuf %d bytes usable%s\n", addr.group.c_str(), addr.port, granted / 2,
                granted / 2 < rcvbuf ? "  (capped by net.core.rmem_max)" : "");
    std::fflush(stdout);

    std::unique_ptr<signals::SignalEngine> engine;
    if (with_signals) engine = std::make_unique<signals::SignalEngine>();
    if (show.empty()) show = {"AAPL", "MSFT", "QQQ"};
    auto books = std::make_unique<book::BookBuilder<book::VectorLevels, MaybeSignals>>(
        22, std::vector<std::uint64_t>{}, MaybeSignals{engine.get()});
    net::FeedHandler feed(*books);
    auto batch = std::make_unique<net::RecvBatch<64, 2048>>();
    auto h_total = std::make_unique<stats::Histogram>();
    auto h_queue = std::make_unique<stats::Histogram>();
    auto h_book = std::make_unique<stats::Histogram>();
    auto h_svc = std::make_unique<stats::Histogram>();

    std::uint64_t datagrams = 0, truncated = 0, missing_ts = 0, clock_anomalies = 0, messages = 0;
    std::uint64_t first_rx = 0, last_rx = 0;
    std::int64_t kernel_drops = 0;
    const auto idle_ns = static_cast<std::uint64_t>(idle_s * 1e9);
    const auto start_ns = static_cast<std::uint64_t>(start_s * 1e9);
    const std::uint64_t t_join = net::monotonic_ns();
    std::uint64_t last_activity = 0;
    bool idle_timeout = false;

    while (!feed.ended()) {
        const int n = batch->receive(sock.fd());
        if (n == 0) {
            if (last_activity && net::monotonic_ns() - last_activity > idle_ns) {
                idle_timeout = true;
                break;
            }
            if (!last_activity && net::monotonic_ns() - t_join > start_ns) {
                std::printf("no packets received (start timeout)\n");
                return 1;
            }
            continue;
        }
        const std::uint64_t t_recv = net::realtime_ns();
        std::uint64_t prev_t_done = t_recv;  // when the previous packet in this batch finished
        last_activity = net::monotonic_ns();
        for (int i = 0; i < n; ++i) {
            ++datagrams;
            std::uint64_t rx_ns = 0;
            std::int64_t drops = 0;
            batch->meta(i, rx_ns, drops);
            if (drops >= 0) kernel_drops = drops;
            if (batch->truncated(i)) {
                ++truncated;
                continue;
            }
            const auto r = feed.on_packet(batch->data(i), batch->size(i));
            if (r.kind != net::PacketKind::data) continue;
            const std::uint64_t t_done = net::realtime_ns();
            const std::uint64_t svc_start = std::max(t_recv, prev_t_done);
            prev_t_done = t_done;
            messages += r.messages;
            if (rx_ns == 0) {
                ++missing_ts;
                continue;
            }
            if (!first_rx) first_rx = rx_ns;
            last_rx = rx_ns;
            if (r.first_ts < from) continue;
            // Realtime clock stepped (WSL time sync): backwards, or forwards by more than 10 s (a deep
            // receive buffer can legitimately hold a >1 s backlog).
            // svc_start >= t_recv, so t_done < svc_start also covers t_done < t_recv.
            if (t_recv < rx_ns || t_done < svc_start || t_done - rx_ns > 10'000'000'000) {
                ++clock_anomalies;
                continue;
            }
            h_total->record(t_done - rx_ns);
            h_queue->record(t_recv - rx_ns);
            h_book->record(t_done - t_recv);
            h_svc->record(t_done - svc_start);
        }
    }

    const auto& s = feed.seq_stats();
    const auto& v = books->violations();
    const double span_s = last_rx > first_rx ? double(last_rx - first_rx) / 1e9 : 0.0;
    std::printf("\nfeed\n");
    std::printf("  datagrams %llu   data packets %llu   messages %llu   %.2f M msgs/s over %.3f s\n",
                (unsigned long long)datagrams, (unsigned long long)s.packets, (unsigned long long)messages,
                span_s > 0 ? double(messages) / span_s / 1e6 : 0.0, span_s);
    std::printf("  gaps %llu (%llu messages lost)   kernel drops %lld   stale %llu   wrong session %llu\n",
                (unsigned long long)s.gaps, (unsigned long long)s.lost_messages, (long long)kernel_drops,
                (unsigned long long)s.stale_packets, (unsigned long long)s.wrong_session);
    std::printf("  malformed %llu   truncated %llu   heartbeats %llu   no timestamp %llu   clock steps %llu\n",
                (unsigned long long)feed.malformed(), (unsigned long long)truncated,
                (unsigned long long)s.heartbeats, (unsigned long long)missing_ts,
                (unsigned long long)clock_anomalies);
    std::printf("  end of session: %s\n", feed.ended() ? "yes" : (idle_timeout ? "NO (idle timeout)" : "NO"));

    std::printf("\nbooks\n");
    std::printf("  live orders %zu   peak %zu   top-of-book hash %016llx\n", books->live_orders(),
                books->peak_live_orders(), (unsigned long long)books->tob_hash());
    std::printf("  unknown ref %llu  overfill %llu  locate mismatch %llu  duplicate add %llu  level mismatch %llu\n",
                (unsigned long long)v.unknown_ref, (unsigned long long)v.overfill,
                (unsigned long long)v.locate_mismatch, (unsigned long long)v.duplicate_add,
                (unsigned long long)v.level_mismatch);
    std::printf("  crossed open %llu  locked open %llu  crossed pre/post/halt %llu (allowed)\n",
                (unsigned long long)v.crossed_open, (unsigned long long)v.locked_open,
                (unsigned long long)v.crossed_other);

    if (engine) {
        std::printf("\nsignals (tau %.0f ms, as of the last message)\n", engine->tau_ns() / 1e6);
        for (const std::string& sym : show) {
            const int loc = books->locate_of(sym);
            if (loc < 0) {
                std::printf("  %-6s not in directory\n", sym.c_str());
                continue;
            }
            const auto l = static_cast<std::uint16_t>(loc);
            const book::Bbo& q = engine->bbo(l);
            const signals::Snapshot x = engine->snapshot(l, engine->last_ts());
            auto r0 = [](double v) { return std::fabs(v) < 0.5 ? 0.0 : v; };
            std::printf("  %-6s %9.4f x %-9.4f  imbalance %+.3f  micro %+.2f bps  ofi %+.0f  flow %+.0f  "
                        "(%llu bbo changes, %llu executions)\n",
                        sym.c_str(), q.bid / 1e4, q.ask / 1e4, x.imbalance, x.micro_bps, r0(x.ofi), r0(x.flow),
                        (unsigned long long)engine->bbo_changes(l), (unsigned long long)engine->executions(l));
        }
    }

    std::printf("\ntick-to-book latency per packet (us)\n");
    print_hist("total", *h_total);
    print_hist("queue", *h_queue);
    print_hist("book", *h_book);
    print_hist("svc", *h_svc);

    const bool ok = feed.ended() && s.gaps == 0 && feed.malformed() == 0 && truncated == 0 && missing_ts == 0 &&
                    v.hard() == 0;
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
