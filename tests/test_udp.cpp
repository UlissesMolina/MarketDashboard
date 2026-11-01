// Loopback multicast smoke test with real sockets: send a few datagrams to our
// own group and check they arrive in order with a kernel receive timestamp.
// Needs a multicast-capable interface (WSL2's eth0 works). If this fails, the
// stage 3 tools cannot work on this machine either.

#include <sys/socket.h>

#include <cstdint>
#include <cstdio>

#include "net/os.hpp"
#include "net/udp.hpp"

namespace {

int failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

}  // namespace

int main() {
    net::McastAddr addr;
    addr.group = "239.255.10.1";
    addr.port = 30999;
    int granted = 0;
    net::Socket rx = net::open_mcast_receiver(addr, 1 << 20, &granted);
    net::Socket tx = net::open_mcast_sender(addr);

    for (unsigned char i = 0; i < 3; ++i) {
        const unsigned char b[4] = {'p', 'i', 'n', i};
        CHECK(::send(tx.fd(), b, sizeof b, 0) == 4);
    }

    net::RecvBatch<8, 64> batch;
    int got = 0;
    bool stamped = true;
    const std::uint64_t deadline = net::monotonic_ns() + 1'000'000'000;
    while (got < 3 && net::monotonic_ns() < deadline) {
        const int n = batch.receive(rx.fd());
        for (int i = 0; i < n; ++i) {
            std::uint64_t rx_ns = 0;
            std::int64_t drops = 0;
            batch.meta(i, rx_ns, drops);
            CHECK(batch.size(i) == 4 && batch.data(i)[3] == got);
            stamped = stamped && rx_ns != 0 && rx_ns <= net::realtime_ns();
            ++got;
        }
    }
    CHECK(got == 3);
    CHECK(stamped);
    std::printf("loopback multicast: %d/3 datagrams, rcvbuf granted %d bytes\n", got, granted);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("udp smoke test passed");
}
