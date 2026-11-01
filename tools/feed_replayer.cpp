// feed_replayer: replays an ITCH file as a MoldUDP64 multicast feed. Each
// packet is a 20-byte header plus a slice of the mmapped file, sent with a
// two-iovec sendmsg(), so messages are never copied in user space.
//
//   feed_replayer <file> [--group A] [--port P] [--iface A] [--session S]
//                 [--rate MSGS_PER_S] [--speed X] [--from HH:MM:SS] [--to HH:MM:SS]
//                 [--limit N] [--max-payload BYTES] [--cpu N]
//
// Pacing. --rate caps the average message rate (default 5e6; 0 = unlimited).
// With --speed X, each packet whose first message is at or after --from is also
// held until its ITCH timestamp, scaled by 1/X and measured from the first such
// packet, so bursts keep their real shape. Those packets only batch messages
// with identical timestamps. Messages before --from go out at the --rate cap,
// which builds the books up to that time. Sends end-of-session at the end.

#include <sys/socket.h>
#include <sys/uio.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "cli.hpp"
#include "itch/mapped_file.hpp"
#include "net/mold.hpp"
#include "net/os.hpp"
#include "net/udp.hpp"

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
                     "[--speed X] [--from HH:MM:SS] [--to HH:MM:SS] [--limit N] [--max-payload B] [--cpu N]\n",
                     argv[0]);
        return 2;
    }
    net::McastAddr addr;
    std::string session = "ITCH50";
    double rate = 5e6, speed = 0;
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
    if (cpu >= 0 && !net::pin_to_cpu(cpu)) std::perror("pin_to_cpu");

    itch::MappedFile file(argv[1]);
    file.prefault();
    net::Socket sock = net::open_mcast_sender(addr);

    const unsigned char* p = file.data();
    const unsigned char* const end = file.end();
    const double ns_per_msg = rate > 0 ? 1e9 / rate : 0;
    std::uint64_t seq = 1, msgs = 0, packets = 0, bytes = 0, send_errors = 0, max_late = 0;
    bool anchored = false, framing_error = false;
    std::uint64_t anchor_wall = 0, anchor_ts = 0;
    const std::uint64_t t_start = net::monotonic_ns();

    while (msgs < limit) {
        net::Span s = net::next_span(p, end, max_payload, ~0ULL, limit - msgs);
        if (s.count == 0) {
            framing_error = p != end;
            break;
        }
        if (s.first_ts >= to) break;
        const bool paced = speed > 0 && s.first_ts >= from;
        if (paced) s = net::next_span(p, end, max_payload, 0, limit - msgs);

        std::uint64_t due = t_start + static_cast<std::uint64_t>(double(msgs) * ns_per_msg);
        if (paced) {
            if (!anchored) {
                anchored = true;
                anchor_wall = net::monotonic_ns();
                anchor_ts = s.first_ts;
            }
            due = std::max(due, anchor_wall + static_cast<std::uint64_t>(double(s.first_ts - anchor_ts) / speed));
        }
        std::uint64_t now = net::monotonic_ns();
        while (now < due) now = net::monotonic_ns();
        max_late = std::max(max_late, now - due);

        net::MoldHeader h = net::make_mold_header(session, seq, s.count);
        iovec iov[2] = {{&h, sizeof h}, {const_cast<unsigned char*>(s.begin), s.bytes}};
        msghdr mh{};
        mh.msg_iov = iov;
        mh.msg_iovlen = 2;
        if (::sendmsg(sock.fd(), &mh, 0) < 0 && send_errors++ == 0) std::perror("sendmsg");

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
    std::printf("max behind sched:  %.1f us\n", double(max_late) / 1e3);
    std::printf("next sequence:     %llu   send errors: %llu%s\n", (unsigned long long)seq,
                (unsigned long long)send_errors, framing_error ? "   FRAMING ERROR in file" : "");
    return (send_errors == 0 && !framing_error) ? 0 : 1;
}
