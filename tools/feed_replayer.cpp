// feed_replayer: replays an ITCH file as a MoldUDP64 multicast feed. Each
// packet is a 20-byte header plus a slice of the mmapped file, sent with a
// two-iovec sendmsg(), so messages are never copied in user space.
//
//   feed_replayer <file> [--group A] [--port P] [--iface A] [--session S]
//                 [--rate MSGS_PER_S] [--speed X] [--from HH:MM:SS] [--to HH:MM:SS]
//                 [--limit N] [--max-payload BYTES] [--max-burst-us US] [--cpu N]
//
// Pacing. --rate is a ceiling on the message rate (default 5e6; 0 = unlimited),
// enforced by a token bucket: after a stall the sender catches up by at most
// --max-burst-us worth of messages (default 50 us), then continues at --rate
// instead of blasting until it is back on the original schedule.
// With --speed X, each packet whose first message is at or after --from is also
// held until its ITCH timestamp, scaled by 1/X and measured from the first such
// packet, so bursts keep their real shape. Those packets only batch messages
// with identical timestamps. A packet sent more than 1 ms behind that schedule
// moves the schedule forward by its lateness (a re-base), so later bursts keep
// their real spacing instead of being compressed. Messages before --from go out
// at the --rate cap, which builds the books up to that time. Lateness (send time
// minus due time) is reported as a distribution. Sends end-of-session at the end.

#include <sys/socket.h>
#include <sys/uio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "cli.hpp"
#include "itch/mapped_file.hpp"
#include "net/mold.hpp"
#include "net/os.hpp"
#include "net/udp.hpp"
#include "stats/histogram.hpp"

namespace {

void spin_until(std::uint64_t t) {
    while (net::monotonic_ns() < t) {
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <itch-file> [--group A] [--port P] [--iface A] [--session S] [--rate N] "
                     "[--speed X] [--from HH:MM:SS] [--to HH:MM:SS] [--limit N] [--max-payload B] "
                     "[--max-burst-us US] [--cpu N]\n",
                     argv[0]);
        return 2;
    }
    net::McastAddr addr;
    std::string session = "ITCH50";
    double rate = 5e6, speed = 0, max_burst_us = 50;
    std::uint64_t from = 0, to = ~0ULL, limit = ~0ULL;
    std::size_t max_payload = net::kDefaultMaxPayload;
    int cpu = -1;
    for (int i = 2; i < argc; ++i) {
        auto opt = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
        if (opt("--group")) addr.group = argv[++i];
        else if (opt("--port")) addr.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (opt("--iface")) addr.iface = argv[++i];
        else if (opt("--session")) session = argv[++i];
        else if (opt("--rate")) rate = std::atof(argv[++i]);
        else if (opt("--speed")) speed = std::atof(argv[++i]);
        else if (opt("--from")) from = cli::parse_hms(argv[++i]);
        else if (opt("--to")) to = cli::parse_hms(argv[++i]);
        else if (opt("--limit")) limit = std::strtoull(argv[++i], nullptr, 10);
        else if (opt("--max-payload")) max_payload = std::strtoull(argv[++i], nullptr, 10);
        else if (opt("--max-burst-us")) max_burst_us = std::atof(argv[++i]);
        else if (opt("--cpu")) cpu = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (session.size() > net::kSessionLen || max_payload > net::kMaxPayload || max_payload == 0) {
        std::fprintf(stderr, "session must be <= %zu chars and max-payload in 1..%zu\n", net::kSessionLen,
                     net::kMaxPayload);
        return 2;
    }
    if (!std::isfinite(rate) || (rate != 0 && rate < 1)) {
        std::fprintf(stderr, "--rate must be 0 (unlimited) or >= 1\n");
        return 2;
    }
    if (!std::isfinite(speed) || speed < 0 || speed > 1e6) {
        std::fprintf(stderr, "--speed must be 0 (off) or in (0, 1e6]\n");
        return 2;
    }
    if (!std::isfinite(max_burst_us) || max_burst_us < 0 || max_burst_us > 1e9) {
        std::fprintf(stderr, "--max-burst-us must be in [0, 1e9]\n");
        return 2;
    }
    if (cpu >= 0 && !net::pin_to_cpu(cpu)) std::perror("pin_to_cpu");

    itch::MappedFile file(argv[1]);
    net::Socket sock = net::open_mcast_sender(addr);

    // Pre-scan: find where the replay stops (--limit, the first message at or
    // after --to, or a framing error) and count the messages. Reading every
    // block touches every page of the range, so the send loop never faults.
    const unsigned char* const end = file.end();
    const unsigned char* stop = file.data();
    std::uint64_t total = 0;
    bool framing_error = false;
    const std::uint64_t t_scan = net::monotonic_ns();
    while (total < limit && stop != end) {
        if (end - stop < 2) {
            framing_error = true;
            break;
        }
        const std::size_t len = itch::load_be16(stop);
        if (len < sizeof(itch::Header) || static_cast<std::size_t>(end - stop) - 2 < len) {
            framing_error = true;
            break;
        }
        if (reinterpret_cast<const itch::Header*>(stop + 2)->timestamp >= to) break;
        stop += 2 + len;
        ++total;
    }
    std::printf("pre-scan:          %llu messages, %.2f GB in %.3f s\n", (unsigned long long)total,
                double(stop - file.data()) / 1e9, double(net::monotonic_ns() - t_scan) / 1e9);
    std::fflush(stdout);

    const unsigned char* p = file.data();
    const double ns_per_msg = rate > 0 ? 1e9 / rate : 0;
    const double max_burst_ns = max_burst_us * 1e3;
    auto h_late = std::make_unique<stats::Histogram>();
    std::uint64_t seq = 1, msgs = 0, packets = 0, bytes = 0, send_errors = 0, late_50us = 0, rebases = 0;
    bool anchored = false;
    std::uint64_t anchor_wall = 0, anchor_ts = 0;
    const std::uint64_t t_start = net::monotonic_ns();
    double next_rate = double(t_start);  // token bucket: when the next packet may go at --rate

    while (p < stop) {
        net::Span s = net::next_span(p, stop, max_payload, ~0ULL, ~0ULL);
        if (s.count == 0) break;  // cannot happen: the pre-scan checked the framing up to stop
        const bool paced = speed > 0 && s.first_ts >= from;
        if (paced) s = net::next_span(p, stop, max_payload, 0, ~0ULL);

        const std::uint64_t now0 = net::monotonic_ns();
        std::uint64_t due = now0;
        if (rate > 0) {
            next_rate = std::max(next_rate, double(now0) - max_burst_ns);  // bounded catch-up
            due = static_cast<std::uint64_t>(next_rate);
        }
        std::uint64_t ts_due = 0;
        if (paced) {
            if (!anchored) {
                anchored = true;
                anchor_wall = now0;
                anchor_ts = s.first_ts;
            }
            const std::uint64_t dts = s.first_ts > anchor_ts ? s.first_ts - anchor_ts : 0;
            ts_due = anchor_wall + static_cast<std::uint64_t>(double(dts) / speed);
            due = std::max(due, ts_due);
        }
        std::uint64_t now = now0;
        while (now < due) now = net::monotonic_ns();
        const std::uint64_t late = now - due;
        h_late->record(late);
        if (late > 50'000) ++late_50us;
        if (paced && now - ts_due > 1'000'000) {  // far behind: move the schedule, keep later spacing
            anchor_wall += now - ts_due;
            ++rebases;
        }

        net::MoldHeader h = net::make_mold_header(session, seq, s.count);
        iovec iov[2] = {{&h, sizeof h}, {const_cast<unsigned char*>(s.begin), s.bytes}};
        msghdr mh{};
        mh.msg_iov = iov;
        mh.msg_iovlen = 2;
        if (::sendmsg(sock.fd(), &mh, 0) < 0 && send_errors++ == 0) std::perror("sendmsg");
        next_rate += double(s.count) * ns_per_msg;

        seq += s.count;
        msgs += s.count;
        ++packets;
        bytes += sizeof h + s.bytes;
        p += s.bytes;
    }
    const double secs = double(net::monotonic_ns() - t_start) / 1e9;

    // End of session, sent a few times since any single datagram can be lost.
    net::MoldHeader eos = net::make_mold_header(session, seq, net::kEndOfSession);
    for (int i = 0; i < 3; ++i) {
        if (::send(sock.fd(), &eos, sizeof eos, 0) < 0 && send_errors++ == 0) std::perror("send");
        spin_until(net::monotonic_ns() + 1'000'000);
    }

    std::printf("replayed to %s:%u session '%s'\n", addr.group.c_str(), addr.port, session.c_str());
    std::printf("messages:          %llu in %llu packets (%.1f msgs/packet), %.2f GB\n", (unsigned long long)msgs,
                (unsigned long long)packets, packets ? double(msgs) / double(packets) : 0.0, double(bytes) / 1e9);
    std::printf("elapsed:           %.3f s   %.2f M msgs/s   %.0f k packets/s\n", secs, double(msgs) / secs / 1e6,
                double(packets) / secs / 1e3);
    auto us = [](std::uint64_t ns) { return double(ns) / 1e3; };
    std::printf("send lateness us:  p50 %.2f  p99 %.2f  p99.9 %.2f  max %.2f   >50us %llu   re-bases %llu\n",
                us(h_late->percentile(0.5)), us(h_late->percentile(0.99)), us(h_late->percentile(0.999)),
                us(h_late->max()), (unsigned long long)late_50us, (unsigned long long)rebases);
    std::printf("next sequence:     %llu   send errors: %llu%s\n", (unsigned long long)seq,
                (unsigned long long)send_errors, framing_error ? "   FRAMING ERROR in file" : "");
    return (send_errors == 0 && !framing_error) ? 0 : 1;
}
