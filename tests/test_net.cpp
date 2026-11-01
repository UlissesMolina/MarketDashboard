// Unit tests for the stage 3 feed pieces that need no sockets: latency
// histogram, MoldUDP64 framing, sequence tracking, and the feed handler.
// Built with ASan/UBSan by build.sh.

#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include <cstring>
#include <string_view>
#include <vector>

#include "net/mold.hpp"
#include "stats/histogram.hpp"
#include "wire.hpp"

namespace {

int failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

using test::Wire;
constexpr std::uint16_t kLoc = 1;

void add(Wire& w, std::uint64_t ref, char side, std::uint32_t price, std::uint64_t ts) {
    w.header('A', kLoc, 0, ts).be(ref, 8).ch(side).be(100, 4).str("TEST", 8).be(price, 4).end();
}

void test_histogram_exact_below_128() {
    stats::Histogram h;
    for (std::uint64_t v = 1; v <= 100; ++v) h.record(v);
    CHECK(h.count() == 100);
    CHECK(h.min() == 1 && h.max() == 100);
    CHECK(h.mean() == 50.5);
    CHECK(h.percentile(0.5) == 50);
    CHECK(h.percentile(0.99) == 99);
    CHECK(h.percentile(1.0) == 100);
    CHECK(h.percentile(0.0) == 1);
}

void test_histogram_bucket_bounds() {
    using H = stats::Histogram;
    for (std::uint64_t v : {0ULL, 127ULL, 128ULL, 255ULL, 256ULL, 257ULL, 383ULL, 1000ULL, 123456789ULL, ~0ULL}) {
        const std::uint64_t lo = H::lower_bound(H::index(v));
        CHECK(lo <= v);
        CHECK(v - lo <= v / 128);  // relative error below 1/128
    }
    CHECK(H::index(~0ULL) == H::kBuckets - 1);
    std::size_t prev = 0;
    for (std::uint64_t v = 0; v < 1'000'000; v += 7) {
        const std::size_t i = H::index(v);
        if (i < prev) {
            CHECK(i >= prev);
            return;
        }
        prev = i;
    }
}

void test_histogram_tail() {
    stats::Histogram h;
    for (int i = 0; i < 990; ++i) h.record(10'000);   // 10 us body
    for (int i = 0; i < 10; ++i) h.record(5'000'000); // 5 ms tail
    const std::uint64_t p50 = h.percentile(0.5);
    CHECK(p50 <= 10'000 && p50 >= 10'000 - 10'000 / 128);
    CHECK(h.percentile(0.95) <= 10'000);
    const std::uint64_t p999 = h.percentile(0.999);
    CHECK(p999 <= 5'000'000 && p999 >= 5'000'000 - 5'000'000 / 128);
    CHECK(h.max() == 5'000'000);
    CHECK(stats::Histogram{}.percentile(0.5) == 0);
}

void test_mold_header_layout() {
    const net::MoldHeader h = net::make_mold_header("ITCH", 0x0102030405060708ULL, 3);
    unsigned char raw[sizeof h];
    std::memcpy(raw, &h, sizeof h);
    CHECK(std::memcmp(raw, "ITCH      ", 10) == 0);
    for (int i = 0; i < 8; ++i) CHECK(raw[10 + i] == i + 1);
    CHECK(raw[18] == 0 && raw[19] == 3);
    CHECK(h.sequence.get() == 0x0102030405060708ULL);
    CHECK(h.count.get() == 3);
    CHECK(net::session_of(h) == "ITCH      ");
}

void test_skip_messages() {
    Wire w;
    add(w, 1, 'B', 100, 1);
    add(w, 2, 'B', 100, 2);
    add(w, 3, 'B', 100, 3);
    CHECK(net::skip_messages(w.data(), w.data_end(), 0) == w.data());
    CHECK(net::skip_messages(w.data(), w.data_end(), 2) == w.data() + 2 * 38);  // 'A' is 36 bytes + 2 length
    CHECK(net::skip_messages(w.data(), w.data_end(), 3) == w.data_end());
    CHECK(net::skip_messages(w.data(), w.data_end(), 4) == nullptr);
    CHECK(net::skip_messages(w.data(), w.data_end() - 1, 3) == nullptr);  // truncated last block
}

}  // namespace

int main() {
    test_histogram_exact_below_128();
    test_histogram_bucket_bounds();
    test_histogram_tail();
    test_mold_header_layout();
    test_skip_messages();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all net tests passed");
}
