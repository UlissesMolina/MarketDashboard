# Stage 3: Multicast Feed + Tick-to-Book Latency — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replay the ITCH file as a MoldUDP64 multicast feed, consume it in real time into the existing order books, and measure tick-to-book latency (kernel receive timestamp → books updated) as a histogram.

**Architecture:** MoldUDP64 message blocks have exactly the ITCH sample file's framing (2-byte BE length + message), so a packet is a 20-byte header plus a contiguous slice of the mmapped file. The replayer sends it with a two-iovec `sendmsg` (no copies). The consumer's per-packet logic (header check, sequence tracking, parse into `BookBuilder`) is a socket-free `FeedHandler` so tests can drive it with dropped, duplicated, and overlapping packets. The tool around it only does `recvmmsg`, reads the kernel's `SO_TIMESTAMPNS` timestamp and `SO_RXQ_OVFL` drop count, and records latency into a fixed-size log-linear histogram.

**Tech Stack:** C++20, g++ 13, Linux sockets (`recvmmsg`, `sendmsg`, IPv4 multicast), WSL2 Ubuntu 24.04. No new libraries.

**Out of scope (YAGNI):** retransmission / gap recovery (Mold request server), multiple sessions, kernel bypass, hardware timestamps. A gap is counted and fails the run.

---

## Ground rules for every task

- **Build/test command** (run from Windows; builds inside WSL):
  `wsl -e bash -lc 'cd /mnt/c/market && ./build.sh test'`
  `build.sh` compiles everything with `-std=c++20 -Wall -Wextra -Wpedantic -Werror`. A warning is a build failure. Initialize every aggregate member explicitly (e.g. `return {kind, 0, 0};`) to avoid `-Wmissing-field-initializers`.
- **Tests** follow the existing style: a plain `main()` with a `CHECK` macro and a `failures` counter (see `tests/test_book.cpp`). Unit tests are built with ASan/UBSan.
- **Data** lives at `~/itch/01302019.NASDAQ_ITCH50` inside WSL (not `/mnt/c`).
- **No `sudo`** from the agent. When a step needs root, ask the user to run it with `! wsl sudo ...`.
- **Commits:** plain messages, **no** `Co-Authored-By` or any Claude attribution.

## File map

| File | Responsibility |
|---|---|
| `include/stats/histogram.hpp` | `stats::Histogram`: fixed-size log-linear latency histogram |
| `include/net/mold.hpp` | MoldUDP64 header struct, `make_mold_header`, `skip_messages`, `next_span` (packetizer) |
| `include/net/seq_tracker.hpp` | `PacketKind`, `SeqTracker`: session lock, gaps, duplicates, partial overlap |
| `include/net/feed_handler.hpp` | `FeedHandler<H>`: header → sequence → `itch::parse` into a handler |
| `include/net/os.hpp` | clocks (`realtime_ns`, `monotonic_ns`), `pin_to_cpu` |
| `include/net/udp.hpp` | RAII `Socket`, multicast sender/receiver setup, `RecvBatch` (recvmmsg + cmsgs) |
| `tools/cli.hpp` | `cli::parse_hms` shared by both tools |
| `tools/feed_replayer.cpp` | file → paced MoldUDP64 multicast |
| `tools/feed_consumer.cpp` | multicast → books + latency report |
| `tools/feed_bench.sh` | runs consumer + replayer together |
| `tests/test_net.cpp` | unit tests for histogram, mold, seq tracker, feed handler (no sockets) |
| `tests/test_udp.cpp` | loopback multicast smoke test (real sockets) |
| `build.sh`, `README.md` | modified |

---

### Task 0: Check the WSL2 network environment

No code. This catches environment problems before any code depends on them.

- [ ] **Step 1: Inspect interfaces, multicast route, and receive-buffer cap**

Run:
```
wsl -e bash -lc 'ip -br link; ip route get 239.1.1.1; sysctl net.core.rmem_max'
```
Expected: `eth0` is listed `UP` with `MULTICAST` in its flags (`ip -br link` doesn't show flags; also run `ip link show eth0`). `ip route get 239.1.1.1` names a device (normally `eth0` via the default route). `rmem_max` is probably `212992`.

- [ ] **Step 2: Ask the user to raise the receive-buffer cap**

A 208 KB socket buffer fills in well under a millisecond at millions of messages per second, so drops would measure the default limit rather than the code. Ask the user to run:
```
! wsl sudo sysctl -w net.core.rmem_max=268435456
```
This lasts until WSL restarts. Record the decision in the README in Task 10. If the user declines, continue anyway: the consumer reports the buffer size it actually got, and drops will appear at lower rates.

---

### Task 1: Latency histogram

**Files:**
- Create: `include/stats/histogram.hpp`
- Create: `tests/test_net.cpp`
- Modify: `build.sh`

- [ ] **Step 1: Write the failing test**

Create `tests/test_net.cpp`:
```cpp
// Unit tests for the stage 3 feed pieces that need no sockets: latency
// histogram, MoldUDP64 framing, sequence tracking, and the feed handler.
// Built with ASan/UBSan by build.sh.

#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "stats/histogram.hpp"

namespace {

int failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

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

}  // namespace

int main() {
    test_histogram_exact_below_128();
    test_histogram_bucket_bounds();
    test_histogram_tail();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all net tests passed");
}
```

In `build.sh`, after the `test_book` compile line, add:
```bash
$CXX "${COMMON[@]}" "${SANITIZE[@]}" -Itests tests/test_net.cpp -o build/test_net
```
and inside the `if [[ "${1:-}" == "test" ]]` block, after `./build/test_book`, add:
```bash
    ./build/test_net
```

- [ ] **Step 2: Run it and confirm it fails**

Run: `wsl -e bash -lc 'cd /mnt/c/market && ./build.sh test'`
Expected: compile error `stats/histogram.hpp: No such file or directory`.

- [ ] **Step 3: Implement**

Create `include/stats/histogram.hpp`:
```cpp
#pragma once

// Log-linear latency histogram (HdrHistogram-style). Values below 128 are
// counted exactly. Above that, each power of two is split into 128 buckets, so a
// reported percentile is at most 1/128 (0.8%) below the true value. Fixed size
// (~58 KB) and no allocation in record(), so it can sit on the hot path.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace stats {

class Histogram {
public:
    static constexpr int kSubBits = 7;
    static constexpr std::uint64_t kSub = std::uint64_t{1} << kSubBits;
    static constexpr std::size_t kBuckets = (64 - kSubBits + 1) * kSub;

    void record(std::uint64_t v) noexcept {
        ++counts_[index(v)];
        ++count_;
        sum_ += v;
        min_ = std::min(min_, v);
        max_ = std::max(max_, v);
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t min() const noexcept { return count_ ? min_ : 0; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
    [[nodiscard]] double mean() const noexcept { return count_ ? double(sum_) / double(count_) : 0.0; }

    // Value at or below which a fraction p of samples fall, to bucket precision
    // (the bucket's lower bound, clamped to [min, max]). p >= 1 returns the exact max.
    [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
        if (count_ == 0) return 0;
        if (p >= 1.0) return max_;
        // The epsilon keeps p * count from rounding up past an exact rank (0.99 * 100).
        const auto rank = std::max<std::uint64_t>(
            1, static_cast<std::uint64_t>(std::ceil(p * double(count_) - 1e-9)));
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= rank) return std::clamp(lower_bound(i), min_, max_);
        }
        return max_;
    }

    static constexpr std::size_t index(std::uint64_t v) noexcept {
        if (v < kSub) return static_cast<std::size_t>(v);
        const int shift = static_cast<int>(std::bit_width(v)) - 1 - kSubBits;
        return static_cast<std::size_t>(shift + 1) * kSub + static_cast<std::size_t>((v >> shift) - kSub);
    }

    static constexpr std::uint64_t lower_bound(std::size_t i) noexcept {
        if (i < kSub) return i;
        const int shift = static_cast<int>(i / kSub) - 1;
        return (static_cast<std::uint64_t>(i % kSub) + kSub) << shift;
    }

private:
    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t max_ = 0;
    std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
};

}  // namespace stats
```

- [ ] **Step 4: Run tests and confirm they pass**

Run: `wsl -e bash -lc 'cd /mnt/c/market && ./build.sh test'`
Expected: ends with `all parser tests passed` (or the parser test's own message), `all book tests passed`, `all net tests passed`.

- [ ] **Step 5: Commit**

```bash
git add include/stats/histogram.hpp tests/test_net.cpp build.sh
git commit -m "Add log-linear latency histogram"
```

---

### Task 2: MoldUDP64 header and message skipping

**Files:**
- Create: `include/net/mold.hpp`
- Modify: `tests/test_net.cpp`

- [ ] **Step 1: Write the failing tests**

In `tests/test_net.cpp`, add includes after `#include "stats/histogram.hpp"`:
```cpp
#include <cstring>
#include <string_view>
#include <vector>

#include "net/mold.hpp"
#include "wire.hpp"
```
Add `using test::Wire;` and a constant after the `CHECK` macro:
```cpp
using test::Wire;
constexpr std::uint16_t kLoc = 1;

void add(Wire& w, std::uint64_t ref, char side, std::uint32_t price, std::uint64_t ts) {
    w.header('A', kLoc, 0, ts).be(ref, 8).ch(side).be(100, 4).str("TEST", 8).be(price, 4).end();
}
```
Add these tests above `main`:
```cpp
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
```
Call both from `main` after the histogram tests:
```cpp
    test_mold_header_layout();
    test_skip_messages();
```

- [ ] **Step 2: Run and confirm it fails**

Run: `wsl -e bash -lc 'cd /mnt/c/market && ./build.sh test'`
Expected: compile error `net/mold.hpp: No such file or directory`.

- [ ] **Step 3: Implement**

Create `include/net/mold.hpp`:
```cpp
#pragma once

// MoldUDP64, Nasdaq's UDP framing for ITCH.
//
// A packet is a 20-byte header followed by `count` message blocks, each a 2-byte
// big-endian length and then the message. That block layout is exactly the
// framing of the ITCH sample files, so a packet's payload is a contiguous slice
// of the file: the replayer sends it straight out of the mmap and the consumer
// hands it to itch::parse unchanged.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "itch/endian.hpp"
#include "itch/messages.hpp"

namespace net {

constexpr std::size_t kSessionLen = 10;
constexpr std::uint16_t kHeartbeat = 0;
constexpr std::uint16_t kEndOfSession = 0xFFFF;
// Message-block bytes per packet. 1500 MTU - 20 IP - 8 UDP - 20 Mold = 1452.
constexpr std::size_t kMaxPayload = 1452;
constexpr std::size_t kDefaultMaxPayload = 1400;

struct MoldHeader {
    char session[kSessionLen];  // space-padded
    itch::be64 sequence;        // sequence number of the first message; a session starts at 1
    itch::be16 count;           // messages in the packet; 0 = heartbeat, 0xFFFF = end of session
};
static_assert(sizeof(MoldHeader) == 20 && alignof(MoldHeader) == 1);

inline void put_be(unsigned char* p, std::uint64_t v, int n) noexcept {
    for (int i = n - 1; i >= 0; --i) *p++ = static_cast<unsigned char>(v >> (8 * i));
}

[[nodiscard]] inline MoldHeader make_mold_header(std::string_view session, std::uint64_t seq,
                                                 std::uint16_t count) noexcept {
    MoldHeader h{};
    for (std::size_t i = 0; i < kSessionLen; ++i) h.session[i] = i < session.size() ? session[i] : ' ';
    put_be(h.sequence.bytes, seq, 8);
    put_be(h.count.bytes, count, 2);
    return h;
}

[[nodiscard]] inline std::string_view session_of(const MoldHeader& h) noexcept {
    return {h.session, kSessionLen};
}

// Skips n message blocks. Returns nullptr if they run past `end`.
[[nodiscard]] inline const unsigned char* skip_messages(const unsigned char* p, const unsigned char* end,
                                                        std::uint64_t n) noexcept {
    for (; n > 0; --n) {
        if (end - p < 2) return nullptr;
        const std::size_t len = itch::load_be16(p);
        if (static_cast<std::size_t>(end - p) - 2 < len) return nullptr;
        p += 2 + len;
    }
    return p;
}

}  // namespace net
```

- [ ] **Step 4: Run tests and confirm they pass**

Run: `wsl -e bash -lc 'cd /mnt/c/market && ./build.sh test'`
Expected: `all net tests passed`.

- [ ] **Step 5: Commit**

```bash
git add include/net/mold.hpp tests/test_net.cpp
git commit -m "Add MoldUDP64 header and message-block skipping"
```

---

### Task 3: Packetizer (`next_span`)

**Files:**
- Modify: `include/net/mold.hpp`
- Modify: `tests/test_net.cpp`

- [ ] **Step 1: Write the failing tests**

Add above `main` in `tests/test_net.cpp`:
```cpp
void test_next_span_payload_limit() {
    Wire w;
    add(w, 1, 'B', 100, 1);
    add(w, 2, 'B', 100, 2);
    add(w, 3, 'B', 100, 3);
    net::Span s = net::next_span(w.data(), w.data_end(), 80, ~0ULL, ~0ULL);
    CHECK(s.begin == w.data() && s.count == 2 && s.bytes == 76 && s.first_ts == 1);
    s = net::next_span(w.data() + s.bytes, w.data_end(), 80, ~0ULL, ~0ULL);
    CHECK(s.count == 1 && s.bytes == 38 && s.first_ts == 3);
    s = net::next_span(w.data_end(), w.data_end(), 80, ~0ULL, ~0ULL);
    CHECK(s.count == 0 && s.bytes == 0);
}

void test_next_span_takes_at_least_one() {
    Wire w;
    add(w, 1, 'B', 100, 1);
    const net::Span s = net::next_span(w.data(), w.data_end(), 10, ~0ULL, ~0ULL);
    CHECK(s.count == 1 && s.bytes == 38);
}

void test_next_span_time_window_and_count() {
    Wire w;
    add(w, 1, 'B', 100, 100);
    add(w, 2, 'B', 100, 100);
    add(w, 3, 'B', 100, 150);
    add(w, 4, 'B', 100, 400);
    net::Span s = net::next_span(w.data(), w.data_end(), 1400, 100, ~0ULL);
    CHECK(s.count == 3);  // 150 - 100 <= 100, 400 - 100 > 100
    s = net::next_span(w.data(), w.data_end(), 1400, 0, ~0ULL);
    CHECK(s.count == 2);  // window 0: identical timestamps only
    s = net::next_span(w.data(), w.data_end(), 1400, ~0ULL, 3);
    CHECK(s.count == 3);  // max_count
}

void test_next_span_truncated() {
    Wire w;
    add(w, 1, 'B', 100, 1);
    add(w, 2, 'B', 100, 2);
    const unsigned char* end = w.data_end() - 1;
    net::Span s = net::next_span(w.data(), end, 1400, ~0ULL, ~0ULL);
    CHECK(s.count == 1);
    s = net::next_span(w.data() + s.bytes, end, 1400, ~0ULL, ~0ULL);
    CHECK(s.count == 0);  // caller sees count 0 with p != end: framing error
}
```
Call them from `main`:
```cpp
    test_next_span_payload_limit();
    test_next_span_takes_at_least_one();
    test_next_span_time_window_and_count();
    test_next_span_truncated();
```

- [ ] **Step 2: Run and confirm it fails**

Run: `wsl -e bash -lc 'cd /mnt/c/market && ./build.sh test'`
Expected: compile error `'Span' is not a member of 'net'`.

- [ ] **Step 3: Implement**

Append inside `namespace net` in `include/net/mold.hpp`, after `skip_messages`:
```cpp
// One packet's worth of message blocks.
struct Span {
    const unsigned char* begin;  // first message block
    std::size_t bytes;           // message-block bytes
    std::uint16_t count;         // messages
    std::uint64_t first_ts;      // ITCH timestamp of the first message
};

// Takes whole message blocks from [p, end) for one packet: as many as fit in
// max_payload bytes, at most max_count, and only while each message's timestamp
// is within max_span_ns of the first one. A paced replay passes 0 so it never
// holds a message back to batch it with a later event. Always takes one message
// if a complete one is available. Returns count == 0 at end of input or when the
// framing at p is truncated.
[[nodiscard]] inline Span next_span(const unsigned char* p, const unsigned char* end, std::size_t max_payload,
                                    std::uint64_t max_span_ns, std::uint64_t max_count) noexcept {
    Span s{p, 0, 0, 0};
    const unsigned char* q = p;
    while (s.count < max_count && s.count < kEndOfSession - 1 && end - q >= 2) {
        const std::size_t len = itch::load_be16(q);
        if (len < sizeof(itch::Header) || static_cast<std::size_t>(end - q) - 2 < len) break;
        const std::uint64_t ts = reinterpret_cast<const itch::Header*>(q + 2)->timestamp;
        if (s.count == 0) {
            s.first_ts = ts;
        } else if (s.bytes + 2 + len > max_payload || ts - s.first_ts > max_span_ns) {
            break;
        }
        s.bytes += 2 + len;
        ++s.count;
        q += 2 + len;
    }
    return s;
}
```

- [ ] **Step 4: Run tests and confirm they pass**

Run: `wsl -e bash -lc 'cd /mnt/c/market && ./build.sh test'`
Expected: `all net tests passed`.

- [ ] **Step 5: Commit**

```bash
git add include/net/mold.hpp tests/test_net.cpp
git commit -m "Add MoldUDP64 packetizer over framed ITCH buffers"
```

---

### Task 4: Sequence tracker

**Files:**
- Create: `include/net/seq_tracker.hpp`
- Modify: `tests/test_net.cpp`

- [ ] **Step 1: Write the failing tests**

Add `#include "net/seq_tracker.hpp"` after the `net/mold.hpp` include. Add above `main`:
```cpp
net::SeqDecision on(net::SeqTracker& t, std::uint64_t seq, std::uint16_t count, std::string_view session = "S") {
    return t.on_header(net::make_mold_header(session, seq, count));
}

void test_seq_in_order_and_gap() {
    net::SeqTracker t;
    CHECK(on(t, 1, 2).kind == net::PacketKind::data && on(t, 3, 2).skip == 0);
    CHECK(t.stats().messages == 4 && t.stats().gaps == 0);
    const net::SeqDecision d = on(t, 8, 1);  // expected 5: 5, 6, 7 lost
    CHECK(d.kind == net::PacketKind::data && d.skip == 0);
    CHECK(t.stats().gaps == 1 && t.stats().lost_messages == 3 && t.stats().messages == 5);
}

void test_seq_stale_and_partial_overlap() {
    net::SeqTracker t;
    on(t, 1, 2);
    on(t, 3, 2);
    CHECK(on(t, 1, 2).kind == net::PacketKind::stale);
    CHECK(on(t, 3, 2).kind == net::PacketKind::stale);
    const net::SeqDecision d = on(t, 4, 3);  // 4 already seen, 5 and 6 new
    CHECK(d.kind == net::PacketKind::data && d.skip == 1);
    CHECK(t.stats().stale_packets == 2 && t.stats().messages == 6 && t.stats().gaps == 0);
}

void test_seq_heartbeat_end_and_session() {
    net::SeqTracker t;
    on(t, 1, 2);
    CHECK(on(t, 3, net::kHeartbeat).kind == net::PacketKind::heartbeat);
    CHECK(t.stats().gaps == 0);
    on(t, 5, net::kHeartbeat);  // expected 3: heartbeat reveals 2 lost at the tail
    CHECK(t.stats().gaps == 1 && t.stats().lost_messages == 2 && t.stats().heartbeats == 2);
    CHECK(on(t, 5, 1, "OTHER").kind == net::PacketKind::wrong_session);
    CHECK(t.stats().wrong_session == 1);
    CHECK(!t.ended());
    CHECK(on(t, 5, net::kEndOfSession).kind == net::PacketKind::end_of_session);
    CHECK(t.ended() && t.stats().gaps == 1);
}
```
Call them from `main`:
```cpp
    test_seq_in_order_and_gap();
    test_seq_stale_and_partial_overlap();
    test_seq_heartbeat_end_and_session();
```

- [ ] **Step 2: Run and confirm it fails**

Run: `wsl -e bash -lc 'cd /mnt/c/market && ./build.sh test'`
Expected: compile error `net/seq_tracker.hpp: No such file or directory`.

- [ ] **Step 3: Implement**

Create `include/net/seq_tracker.hpp`:
```cpp
#pragma once

// Sequence-number bookkeeping for one MoldUDP64 session, without recovery: a
// gap is counted and its messages are lost, so the books downstream can no
// longer be trusted (their unknown-ref counter usually shows it too).
// The first session name seen is locked in; other sessions are ignored.

#include <cstdint>
#include <string>
#include <string_view>

#include "net/mold.hpp"

namespace net {

enum class PacketKind { data, heartbeat, end_of_session, stale, wrong_session, malformed };

struct SeqStats {
    std::uint64_t packets = 0;        // data packets with at least one new message
    std::uint64_t messages = 0;       // new messages
    std::uint64_t heartbeats = 0;
    std::uint64_t gaps = 0;           // times the sequence jumped forward
    std::uint64_t lost_messages = 0;  // messages skipped over by those jumps
    std::uint64_t stale_packets = 0;  // packets with nothing new (duplicates, late arrivals)
    std::uint64_t wrong_session = 0;
};

struct SeqDecision {
    PacketKind kind;
    std::uint64_t skip;  // data: leading messages already seen, to be skipped
};

class SeqTracker {
public:
    SeqDecision on_header(const MoldHeader& h) {
        const std::string_view session = session_of(h);
        if (session_.empty()) {
            session_.assign(session);
        } else if (session != session_) {
            ++s_.wrong_session;
            return {PacketKind::wrong_session, 0};
        }
        const std::uint64_t seq = h.sequence.get();
        const std::uint16_t count = h.count.get();
        if (count == kEndOfSession) {
            note_jump(seq);
            ended_ = true;
            return {PacketKind::end_of_session, 0};
        }
        if (count == kHeartbeat) {
            note_jump(seq);
            ++s_.heartbeats;
            return {PacketKind::heartbeat, 0};
        }
        if (seq + count <= next_) {
            ++s_.stale_packets;
            return {PacketKind::stale, 0};
        }
        note_jump(seq);
        const std::uint64_t skip = next_ - seq;  // > 0 only on partial overlap
        next_ = seq + count;
        ++s_.packets;
        s_.messages += count - skip;
        return {PacketKind::data, skip};
    }

    [[nodiscard]] const SeqStats& stats() const noexcept { return s_; }
    [[nodiscard]] bool ended() const noexcept { return ended_; }
    [[nodiscard]] std::uint64_t next_expected() const noexcept { return next_; }

private:
    // Heartbeats and end-of-session carry the next sequence number, so they
    // reveal a gap at the tail just like a data packet does.
    void note_jump(std::uint64_t seq) {
        if (seq > next_) {
            ++s_.gaps;
            s_.lost_messages += seq - next_;
            next_ = seq;
        }
    }

    std::string session_;
    std::uint64_t next_ = 1;
    SeqStats s_;
    bool ended_ = false;
};

}  // namespace net
```
Note: `skip = next_ - seq` is safe because `note_jump` has already raised `next_` to at least `seq`.

- [ ] **Step 4: Run tests and confirm they pass**

Run: `wsl -e bash -lc 'cd /mnt/c/market && ./build.sh test'`
Expected: `all net tests passed`.

- [ ] **Step 5: Commit**

```bash
git add include/net/seq_tracker.hpp tests/test_net.cpp
git commit -m "Add MoldUDP64 sequence tracker with gap and overlap handling"
```

---

### Task 5: Feed handler (packets → books)

**Files:**
- Create: `include/net/feed_handler.hpp`
- Modify: `tests/test_net.cpp`

- [ ] **Step 1: Write the failing tests**

Add includes to `tests/test_net.cpp`:
```cpp
#include "book/book_builder.hpp"
#include "itch/parser.hpp"
#include "net/feed_handler.hpp"
```
Add helpers and tests above `main`:
```cpp
using Book = book::BookBuilder<book::VectorLevels>;
using Packet = std::vector<unsigned char>;

// 20 adds (alternating sides, never crossing), 20 partial executions, 20 deletes.
Wire feed_messages() {
    Wire w;
    for (std::uint64_t i = 1; i <= 20; ++i)
        add(w, i, i % 2 ? 'B' : 'S', i % 2 ? static_cast<std::uint32_t>(1000 - i) : static_cast<std::uint32_t>(2000 + i), i);
    for (std::uint64_t i = 1; i <= 20; ++i) w.header('E', kLoc, 0, 100 + i).be(i, 8).be(40, 4).be(i, 8).end();
    for (std::uint64_t i = 1; i <= 20; ++i) w.header('D', kLoc, 0, 200 + i).be(i, 8).end();
    return w;
}

Packet packet(std::string_view session, std::uint64_t seq, std::uint16_t count,
              const unsigned char* body = nullptr, std::size_t bytes = 0) {
    const net::MoldHeader h = net::make_mold_header(session, seq, count);
    const auto* hp = reinterpret_cast<const unsigned char*>(&h);
    Packet out(hp, hp + sizeof h);
    if (bytes) out.insert(out.end(), body, body + bytes);
    return out;
}

std::vector<Packet> packetize(const unsigned char* p, const unsigned char* end, std::size_t max_payload,
                              std::uint64_t seq = 1) {
    std::vector<Packet> pkts;
    for (;;) {
        const net::Span s = net::next_span(p, end, max_payload, ~0ULL, ~0ULL);
        if (s.count == 0) break;
        pkts.push_back(packet("TEST", seq, s.count, s.begin, s.bytes));
        seq += s.count;
        p += s.bytes;
    }
    return pkts;
}

std::uint64_t reference_hash(const Wire& w) {
    Book b(8);
    itch::parse(w.data(), w.data_end(), b);
    return b.tob_hash();
}

void test_feed_in_order_matches_direct_parse() {
    const Wire w = feed_messages();
    const auto pkts = packetize(w.data(), w.data_end(), 80);
    CHECK(pkts.size() > 20);
    Book b(8);
    net::FeedHandler f(b);
    std::uint64_t msgs = 0;
    for (std::size_t i = 0; i < pkts.size(); ++i) {
        const auto r = f.on_packet(pkts[i].data(), pkts[i].size());
        CHECK(r.kind == net::PacketKind::data);
        if (i == 0) CHECK(r.first_ts == 1);
        msgs += r.messages;
    }
    CHECK(msgs == 60);
    CHECK(f.seq_stats().messages == 60 && f.seq_stats().gaps == 0);
    CHECK(f.malformed() == 0);
    CHECK(b.violations().hard() == 0);
    CHECK(b.tob_hash() == reference_hash(w));
}

void test_feed_duplicates_are_ignored() {
    const Wire w = feed_messages();
    const auto pkts = packetize(w.data(), w.data_end(), 80);
    Book b(8);
    net::FeedHandler f(b);
    for (const auto& p : pkts) {
        f.on_packet(p.data(), p.size());
        CHECK(f.on_packet(p.data(), p.size()).kind == net::PacketKind::stale);
    }
    CHECK(f.seq_stats().stale_packets == pkts.size());
    CHECK(b.tob_hash() == reference_hash(w));
}

void test_feed_drop_is_a_gap() {
    const Wire w = feed_messages();
    const auto pkts = packetize(w.data(), w.data_end(), 80);
    Book b(8);
    net::FeedHandler f(b);
    for (std::size_t i = 0; i < pkts.size(); ++i)
        if (i != 1) f.on_packet(pkts[i].data(), pkts[i].size());  // packet 1 holds adds 3 and 4
    CHECK(f.seq_stats().gaps == 1 && f.seq_stats().lost_messages == 2);
    CHECK(b.violations().unknown_ref > 0);  // later exec/delete of refs 3 and 4
    CHECK(b.tob_hash() != reference_hash(w));
}

void test_feed_partial_overlap() {
    const Wire w = feed_messages();
    const net::Span two = net::next_span(w.data(), w.data_end(), 1400, ~0ULL, 2);
    const net::Span four = net::next_span(w.data(), w.data_end(), 1400, ~0ULL, 4);
    Book b(8);
    net::FeedHandler f(b);
    const Packet p2 = packet("TEST", 1, 2, two.begin, two.bytes);
    const Packet p4 = packet("TEST", 1, 4, four.begin, four.bytes);
    CHECK(f.on_packet(p2.data(), p2.size()).messages == 2);
    const auto r = f.on_packet(p4.data(), p4.size());
    CHECK(r.kind == net::PacketKind::data && r.messages == 2 && r.first_ts == 3);
    for (const auto& p : packetize(w.data() + four.bytes, w.data_end(), 80, 5)) f.on_packet(p.data(), p.size());
    CHECK(f.seq_stats().messages == 60 && f.malformed() == 0);
    CHECK(b.tob_hash() == reference_hash(w));
}

void test_feed_control_and_malformed() {
    Wire w;
    add(w, 1, 'B', 100, 1);
    Book b(8);
    net::FeedHandler f(b);
    const Packet hb = packet("TEST", 1, net::kHeartbeat);
    CHECK(f.on_packet(hb.data(), hb.size()).kind == net::PacketKind::heartbeat);
    const Packet other = packet("OTHER", 1, net::kHeartbeat);
    CHECK(f.on_packet(other.data(), other.size()).kind == net::PacketKind::wrong_session);
    CHECK(f.on_packet(hb.data(), 10).kind == net::PacketKind::malformed);  // shorter than a header
    const Packet lying = packet("TEST", 1, 5, w.data(), 38);  // claims 5 messages, carries 1
    CHECK(f.on_packet(lying.data(), lying.size()).kind == net::PacketKind::data);
    CHECK(f.malformed() == 2);
    const Packet eos = packet("TEST", 6, net::kEndOfSession);
    CHECK(f.on_packet(eos.data(), eos.size()).kind == net::PacketKind::end_of_session);
    CHECK(f.ended() && f.seq_stats().gaps == 0);
}
```
Call them from `main`:
```cpp
    test_feed_in_order_matches_direct_parse();
    test_feed_duplicates_are_ignored();
    test_feed_drop_is_a_gap();
    test_feed_partial_overlap();
    test_feed_control_and_malformed();
```

- [ ] **Step 2: Run and confirm it fails**

Run: `wsl -e bash -lc 'cd /mnt/c/market && ./build.sh test'`
Expected: compile error `net/feed_handler.hpp: No such file or directory`.

- [ ] **Step 3: Implement**

Create `include/net/feed_handler.hpp`:
```cpp
#pragma once

// The consumer's per-packet logic, kept free of sockets so tests can drive it:
// check the MoldUDP64 header, track sequence numbers, skip messages already
// seen, and parse the new ones into any itch::parse handler (normally a
// BookBuilder). A packet whose message blocks do not match its header's
// count is counted as malformed.

#include <cstddef>
#include <cstdint>

#include "itch/parser.hpp"
#include "net/mold.hpp"
#include "net/seq_tracker.hpp"

namespace net {

template <class Handler>
class FeedHandler {
public:
    struct Result {
        PacketKind kind;
        std::uint64_t first_ts;  // data: ITCH timestamp of the first new message
        std::uint64_t messages;  // data: messages applied
    };

    explicit FeedHandler(Handler& h) : h_(h) {}

    Result on_packet(const unsigned char* pkt, std::size_t len) {
        if (len < sizeof(MoldHeader)) {
            ++malformed_;
            return {PacketKind::malformed, 0, 0};
        }
        const auto& hdr = *reinterpret_cast<const MoldHeader*>(pkt);
        const SeqDecision d = seq_.on_header(hdr);
        if (d.kind != PacketKind::data) return {d.kind, 0, 0};

        const unsigned char* const end = pkt + len;
        const unsigned char* p = skip_messages(pkt + sizeof(MoldHeader), end, d.skip);
        if (!p) {
            ++malformed_;
            return {PacketKind::malformed, 0, 0};
        }
        Result r{PacketKind::data, 0, 0};
        if (static_cast<std::size_t>(end - p) >= 2 + sizeof(itch::Header))
            r.first_ts = reinterpret_cast<const itch::Header*>(p + 2)->timestamp;
        const itch::ParseResult pr = itch::parse(p, end, h_);
        r.messages = pr.messages;
        if (pr.error != itch::ParseError::none || pr.messages != hdr.count.get() - d.skip) ++malformed_;
        return r;
    }

    [[nodiscard]] const SeqStats& seq_stats() const noexcept { return seq_.stats(); }
    [[nodiscard]] bool ended() const noexcept { return seq_.ended(); }
    [[nodiscard]] std::uint64_t malformed() const noexcept { return malformed_; }

private:
    Handler& h_;
    SeqTracker seq_;
    std::uint64_t malformed_ = 0;
};

}  // namespace net
```

- [ ] **Step 4: Run tests and confirm they pass**

Run: `wsl -e bash -lc 'cd /mnt/c/market && ./build.sh test'`
Expected: `all net tests passed`. If `test_feed_drop_is_a_gap` fails on `lost_messages == 2`, print `pkts[1]`'s count: with `max_payload` 80, packet 1 must hold exactly adds 3 and 4 (2 × 38 = 76 bytes).

- [ ] **Step 5: Commit**

```bash
git add include/net/feed_handler.hpp tests/test_net.cpp
git commit -m "Add socket-free feed handler: Mold packets into order books"
```

---

### Task 6: Clocks, CPU pinning, multicast sockets, loopback smoke test

**Files:**
- Create: `include/net/os.hpp`
- Create: `include/net/udp.hpp`
- Create: `tests/test_udp.cpp`
- Modify: `build.sh`

- [ ] **Step 1: Write the failing test**

Create `tests/test_udp.cpp`:
```cpp
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
```
In `build.sh`, after the `test_net` compile line, add:
```bash
$CXX "${COMMON[@]}" "${SANITIZE[@]}" -Itests tests/test_udp.cpp -o build/test_udp
```
and in the test block, after `./build/test_net`:
```bash
    ./build/test_udp
```

- [ ] **Step 2: Run and confirm it fails**

Run: `wsl -e bash -lc 'cd /mnt/c/market && ./build.sh test'`
Expected: compile error `net/os.hpp: No such file or directory`.

- [ ] **Step 3: Implement `os.hpp`**

Create `include/net/os.hpp`:
```cpp
#pragma once

// Clocks and CPU pinning for the feed tools (Linux).
// The kernel stamps received packets with CLOCK_REALTIME (SO_TIMESTAMPNS), so
// latency measured from that stamp must use realtime_ns(). Pacing uses
// monotonic_ns(). Both go through the vDSO (tens of ns, no syscall).

#include <sched.h>
#include <time.h>

#include <cstdint>

namespace net {

[[nodiscard]] inline std::uint64_t to_ns(const timespec& t) noexcept {
    return static_cast<std::uint64_t>(t.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(t.tv_nsec);
}

[[nodiscard]] inline std::uint64_t realtime_ns() noexcept {
    timespec t;
    ::clock_gettime(CLOCK_REALTIME, &t);
    return to_ns(t);
}

[[nodiscard]] inline std::uint64_t monotonic_ns() noexcept {
    timespec t;
    ::clock_gettime(CLOCK_MONOTONIC, &t);
    return to_ns(t);
}

// Pins the calling thread to one CPU. Needs no privileges.
inline bool pin_to_cpu(int cpu) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return ::sched_setaffinity(0, sizeof set, &set) == 0;
}

}  // namespace net
```

- [ ] **Step 4: Implement `udp.hpp`**

Create `include/net/udp.hpp`:
```cpp
#pragma once

// IPv4 UDP multicast sockets for the replayer and consumer (Linux).
//
// On one host the replayer sends with IP_MULTICAST_LOOP on, and the kernel
// hands a copy to local group members itself. The consumer asks for the
// kernel's receive timestamp (SO_TIMESTAMPNS) and the socket's cumulative
// drop count (SO_RXQ_OVFL). Both arrive as control messages with each datagram.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "net/os.hpp"

namespace net {

struct McastAddr {
    std::string group = "239.1.1.1";
    std::uint16_t port = 30001;
    std::string iface = "0.0.0.0";  // local interface address; 0.0.0.0 lets the routing table pick
};

class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) noexcept : fd_(fd) {}
    Socket(Socket&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { reset(); }

    [[nodiscard]] int fd() const noexcept { return fd_; }

private:
    void reset() noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    int fd_ = -1;
};

namespace detail {

[[noreturn]] inline void fail(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

inline in_addr parse_ip(const std::string& s) {
    in_addr a{};
    if (::inet_pton(AF_INET, s.c_str(), &a) != 1) throw std::invalid_argument("bad IPv4 address: " + s);
    return a;
}

template <class T>
void setopt(int fd, int level, int name, const T& v, const char* what) {
    if (::setsockopt(fd, level, name, &v, sizeof v) != 0) fail(what);
}

}  // namespace detail

// Connected socket: send()/sendmsg() go to the group without an address each time.
inline Socket open_mcast_sender(const McastAddr& a, int ttl = 1) {
    Socket s(::socket(AF_INET, SOCK_DGRAM, 0));
    if (s.fd() < 0) detail::fail("socket");
    const in_addr iface = detail::parse_ip(a.iface);
    const int loop = 1;
    detail::setopt(s.fd(), IPPROTO_IP, IP_MULTICAST_IF, iface, "IP_MULTICAST_IF");
    detail::setopt(s.fd(), IPPROTO_IP, IP_MULTICAST_TTL, ttl, "IP_MULTICAST_TTL");
    detail::setopt(s.fd(), IPPROTO_IP, IP_MULTICAST_LOOP, loop, "IP_MULTICAST_LOOP");
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(a.port);
    dst.sin_addr = detail::parse_ip(a.group);
    if (::connect(s.fd(), reinterpret_cast<const sockaddr*>(&dst), sizeof dst) != 0)
        detail::fail("connect " + a.group);
    return s;
}

// Non-blocking member of the group. `granted` receives the buffer size the
// kernel actually set: it caps the request at net.core.rmem_max and reports
// double the usable size.
inline Socket open_mcast_receiver(const McastAddr& a, int rcvbuf, int* granted = nullptr) {
    Socket s(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0));
    if (s.fd() < 0) detail::fail("socket");
    const int one = 1;
    detail::setopt(s.fd(), SOL_SOCKET, SO_REUSEADDR, one, "SO_REUSEADDR");
    detail::setopt(s.fd(), SOL_SOCKET, SO_RCVBUF, rcvbuf, "SO_RCVBUF");
    detail::setopt(s.fd(), SOL_SOCKET, SO_TIMESTAMPNS, one, "SO_TIMESTAMPNS");
    detail::setopt(s.fd(), SOL_SOCKET, SO_RXQ_OVFL, one, "SO_RXQ_OVFL");
    const in_addr group = detail::parse_ip(a.group);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(a.port);
    local.sin_addr = group;  // bind to the group: only its traffic arrives here
    if (::bind(s.fd(), reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0)
        detail::fail("bind " + a.group);
    ip_mreq m{};
    m.imr_multiaddr = group;
    m.imr_interface = detail::parse_ip(a.iface);
    detail::setopt(s.fd(), IPPROTO_IP, IP_ADD_MEMBERSHIP, m, "IP_ADD_MEMBERSHIP");
    if (granted) {
        socklen_t len = sizeof *granted;
        if (::getsockopt(s.fd(), SOL_SOCKET, SO_RCVBUF, granted, &len) != 0) detail::fail("getsockopt SO_RCVBUF");
    }
    return s;
}

// One recvmmsg() batch of up to N datagrams of up to MaxLen bytes, with each
// datagram's kernel timestamp and socket drop counter.
template <std::size_t N = 64, std::size_t MaxLen = 2048>
class RecvBatch {
public:
    RecvBatch() {
        for (std::size_t i = 0; i < N; ++i) {
            iov_[i] = {bufs_[i].data(), MaxLen};
            msgs_[i].msg_hdr.msg_iov = &iov_[i];
            msgs_[i].msg_hdr.msg_iovlen = 1;
            msgs_[i].msg_hdr.msg_control = ctrl_[i].b;
        }
    }

    // Datagrams received; 0 if none are ready. Throws on real errors.
    int receive(int fd) {
        for (auto& m : msgs_) m.msg_hdr.msg_controllen = kCtrl;  // the kernel overwrites it
        const int n = ::recvmmsg(fd, msgs_.data(), N, MSG_DONTWAIT, nullptr);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
            detail::fail("recvmmsg");
        }
        return n;
    }

    [[nodiscard]] const unsigned char* data(int i) const noexcept { return bufs_[i].data(); }
    [[nodiscard]] std::size_t size(int i) const noexcept { return msgs_[i].msg_len; }
    [[nodiscard]] bool truncated(int i) const noexcept { return msgs_[i].msg_hdr.msg_flags & MSG_TRUNC; }

    // rx_ns: kernel receive time (CLOCK_REALTIME ns), 0 if absent.
    // drops: the socket's cumulative drop count, -1 if absent.
    void meta(int i, std::uint64_t& rx_ns, std::int64_t& drops) {
        rx_ns = 0;
        drops = -1;
        msghdr& h = msgs_[i].msg_hdr;
        for (cmsghdr* c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c)) {
            if (c->cmsg_level != SOL_SOCKET) continue;
            if (c->cmsg_type == SCM_TIMESTAMPNS) {
                timespec t;
                std::memcpy(&t, CMSG_DATA(c), sizeof t);
                rx_ns = to_ns(t);
            } else if (c->cmsg_type == SO_RXQ_OVFL) {
                std::uint32_t d;
                std::memcpy(&d, CMSG_DATA(c), sizeof d);
                drops = d;
            }
        }
    }

private:
    static constexpr std::size_t kCtrl = 256;
    struct alignas(cmsghdr) Ctrl {
        unsigned char b[kCtrl];
    };
    std::array<mmsghdr, N> msgs_{};
    std::array<iovec, N> iov_{};
    std::array<std::array<unsigned char, MaxLen>, N> bufs_{};
    std::array<Ctrl, N> ctrl_{};
};

}  // namespace net
```

- [ ] **Step 5: Run tests and confirm they pass**

Run: `wsl -e bash -lc 'cd /mnt/c/market && ./build.sh test'`
Expected: `loopback multicast: 3/3 datagrams, rcvbuf granted ... bytes` and `udp smoke test passed`.
If `got == 3` fails, the problem is the environment, not the code. Check `ip route get 239.255.10.1` (Task 0). If no route goes through a multicast-capable interface, stop and ask the user before working around it (e.g. adding a route needs `sudo`).

- [ ] **Step 6: Commit**

```bash
git add include/net/os.hpp include/net/udp.hpp tests/test_udp.cpp build.sh
git commit -m "Add multicast sockets, recvmmsg batch with kernel timestamps, loopback test"
```

---

### Task 7: Replayer tool

**Files:**
- Create: `tools/cli.hpp`
- Create: `tools/feed_replayer.cpp`
- Modify: `build.sh`

- [ ] **Step 1: Add the shared CLI helper**

Create `tools/cli.hpp`:
```cpp
#pragma once

// Small command-line helpers shared by the feed tools.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace cli {

// "HH:MM:SS" -> nanoseconds since midnight (ITCH timestamp units). Exits on bad input.
inline std::uint64_t parse_hms(const char* s) {
    int h = 0, m = 0, sec = 0;
    if (std::sscanf(s, "%d:%d:%d", &h, &m, &sec) != 3 || h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) {
        std::fprintf(stderr, "bad time '%s' (want HH:MM:SS)\n", s);
        std::exit(2);
    }
    return ((static_cast<std::uint64_t>(h) * 60 + static_cast<std::uint64_t>(m)) * 60 + static_cast<std::uint64_t>(sec)) *
           1'000'000'000ULL;
}

}  // namespace cli
```

- [ ] **Step 2: Write the replayer**

Create `tools/feed_replayer.cpp`:
```cpp
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
```

In `build.sh`, after the `book_replay` compile line, add:
```bash
$CXX "${COMMON[@]}" "${RELEASE[@]}" tools/feed_replayer.cpp -o build/feed_replayer
```

- [ ] **Step 3: Build and smoke-run against the real file**

Run: `wsl -e bash -lc 'cd /mnt/c/market && ./build.sh test && ./build/feed_replayer ~/itch/01302019.NASDAQ_ITCH50 --limit 1000000 --rate 1e6'`
Expected: all tests pass. The replayer prints `messages: 1000000 in ... packets`, elapsed about 1.0 s, about 1.00 M msgs/s, `send errors: 0`, exit 0. No consumer is listening yet, so the datagrams are simply dropped.

- [ ] **Step 4: Commit**

```bash
git add tools/cli.hpp tools/feed_replayer.cpp build.sh
git commit -m "Add MoldUDP64 multicast replayer with rate and timestamp pacing"
```

---

### Task 8: Consumer tool

**Files:**
- Create: `tools/feed_consumer.cpp`
- Create: `tools/feed_bench.sh`
- Modify: `build.sh`

- [ ] **Step 1: Write the consumer**

Create `tools/feed_consumer.cpp`:
```cpp
// feed_consumer: joins a MoldUDP64 ITCH multicast feed, builds every symbol's
// book as packets arrive, and measures tick-to-book latency: from the kernel's
// receive timestamp on a datagram to the moment its last message has been
// applied to the books.
//
//   feed_consumer [--group A] [--port P] [--iface A] [--rcvbuf BYTES] [--cpu N]
//                 [--from HH:MM:SS] [--idle-timeout SECONDS]
//
// Latency is reported in two parts plus the total:
//   queue = recvmmsg() returned - kernel receive    (waiting in the socket buffer)
//   book  = books updated       - recvmmsg() returned (parse + book updates, including
//                                                      packets ahead of it in the batch)
// Only packets whose first message is at or after --from are measured.
// Exits nonzero on any gap, malformed or truncated packet, hard book invariant
// violation, or if no end-of-session packet arrives.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "book/book_builder.hpp"
#include "cli.hpp"
#include "net/feed_handler.hpp"
#include "net/os.hpp"
#include "net/udp.hpp"
#include "stats/histogram.hpp"

namespace {

void print_hist(const char* name, const stats::Histogram& h) {
    auto us = [](std::uint64_t ns) { return double(ns) / 1e3; };
    std::printf("  %-5s n=%-9llu min %7.2f  p50 %7.2f  p90 %7.2f  p99 %8.2f  p99.9 %8.2f  p99.99 %9.2f  max %9.2f  mean %7.2f\n",
                name, (unsigned long long)h.count(), us(h.min()), us(h.percentile(0.5)), us(h.percentile(0.9)),
                us(h.percentile(0.99)), us(h.percentile(0.999)), us(h.percentile(0.9999)), us(h.max()),
                h.mean() / 1e3);
}

}  // namespace

int main(int argc, char** argv) {
    net::McastAddr addr;
    int rcvbuf = 256 << 20, cpu = -1;
    std::uint64_t from = 0;
    double idle_s = 5;
    for (int i = 1; i < argc; ++i) {
        auto opt = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
        if (opt("--group")) addr.group = argv[++i];
        else if (opt("--port")) addr.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (opt("--iface")) addr.iface = argv[++i];
        else if (opt("--rcvbuf")) rcvbuf = std::atoi(argv[++i]);
        else if (opt("--cpu")) cpu = std::atoi(argv[++i]);
        else if (opt("--from")) from = cli::parse_hms(argv[++i]);
        else if (opt("--idle-timeout")) idle_s = std::atof(argv[++i]);
        else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (cpu >= 0 && !net::pin_to_cpu(cpu)) std::perror("pin_to_cpu");

    int granted = 0;
    net::Socket sock = net::open_mcast_receiver(addr, rcvbuf, &granted);
    std::printf("joined %s:%u   rcvbuf %d bytes usable%s\n", addr.group.c_str(), addr.port, granted / 2,
                granted / 2 < rcvbuf ? "  (capped by net.core.rmem_max)" : "");
    std::fflush(stdout);

    auto books = std::make_unique<book::BookBuilder<book::VectorLevels>>();
    net::FeedHandler feed(*books);
    auto batch = std::make_unique<net::RecvBatch<64, 2048>>();
    auto h_total = std::make_unique<stats::Histogram>();
    auto h_queue = std::make_unique<stats::Histogram>();
    auto h_book = std::make_unique<stats::Histogram>();

    std::uint64_t datagrams = 0, truncated = 0, missing_ts = 0, clock_anomalies = 0, messages = 0;
    std::uint64_t first_rx = 0, last_rx = 0;
    std::int64_t kernel_drops = 0;
    const auto idle_ns = static_cast<std::uint64_t>(idle_s * 1e9);
    std::uint64_t last_activity = 0;
    bool idle_timeout = false;

    while (!feed.ended()) {
        const int n = batch->receive(sock.fd());
        if (n == 0) {
            if (last_activity && net::monotonic_ns() - last_activity > idle_ns) {
                idle_timeout = true;
                break;
            }
            continue;
        }
        const std::uint64_t t_recv = net::realtime_ns();
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
            messages += r.messages;
            if (rx_ns == 0) {
                ++missing_ts;
                continue;
            }
            if (!first_rx) first_rx = rx_ns;
            last_rx = rx_ns;
            if (r.first_ts < from) continue;
            if (t_recv < rx_ns || t_done < t_recv) {  // realtime clock stepped (WSL time sync)
                ++clock_anomalies;
                continue;
            }
            h_total->record(t_done - rx_ns);
            h_queue->record(t_recv - rx_ns);
            h_book->record(t_done - t_recv);
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

    std::printf("\ntick-to-book latency per packet (us)\n");
    print_hist("total", *h_total);
    print_hist("queue", *h_queue);
    print_hist("book", *h_book);

    const bool ok = feed.ended() && s.gaps == 0 && feed.malformed() == 0 && truncated == 0 && v.hard() == 0;
    return ok ? 0 : 1;
}
```

- [ ] **Step 2: Write the bench script**

Create `tools/feed_bench.sh`:
```bash
#!/usr/bin/env bash
# Runs feed_consumer and feed_replayer together on this host and prints both reports.
#
#   tools/feed_bench.sh FILE [replayer args...]
#
# Environment: CONSUMER_ARGS (extra consumer flags), CONSUMER_CPU (default 2),
# REPLAYER_CPU (default 4). Exits with the consumer's status.
set -uo pipefail
cd "$(dirname "$0")/.."

file=$1
shift
out=$(mktemp)
# shellcheck disable=SC2086  # CONSUMER_ARGS is meant to split into flags
./build/feed_consumer --cpu "${CONSUMER_CPU:-2}" ${CONSUMER_ARGS:-} >"$out" 2>&1 &
consumer=$!
trap 'kill "$consumer" 2>/dev/null; rm -f "$out"' EXIT
sleep 1  # let the consumer join the group before the first packet

echo "--- replayer"
./build/feed_replayer "$file" --cpu "${REPLAYER_CPU:-4}" "$@"
replayer=$?

wait "$consumer"
status=$?
echo "--- consumer (exit $status)"
cat "$out"
[[ $replayer -ne 0 ]] && echo "replayer exited $replayer"
exit "$status"
```

In `build.sh`, after the `feed_replayer` line, add:
```bash
$CXX "${COMMON[@]}" "${RELEASE[@]}" tools/feed_consumer.cpp -o build/feed_consumer
```

- [ ] **Step 3: Build and do a small end-to-end run**

Run: `wsl -e bash -lc 'cd /mnt/c/market && ./build.sh test && bash tools/feed_bench.sh ~/itch/01302019.NASDAQ_ITCH50 --limit 2000000 --rate 1e6'`
Expected: the replayer reports 2,000,000 messages. The consumer exits 0 with `messages 2000000`, `gaps 0`, `kernel drops 0`, `end of session: yes`, every hard violation 0, and three latency lines with `n` equal to the data-packet count. If the consumer hangs, the replayer's packets are not reaching it. Rerun `build/test_udp` and check whether `--iface` is needed.

- [ ] **Step 4: Commit**

```bash
git add tools/feed_consumer.cpp tools/feed_bench.sh build.sh
git commit -m "Add multicast feed consumer with tick-to-book latency histograms"
```

---

### Task 9: Full-day correctness and latency measurements

No code unless a run exposes a bug. If one does, switch to superpowers:systematic-debugging and add a failing unit test to `tests/test_net.cpp` before fixing it.

- [ ] **Step 0: Check the receive buffer**

Run any short bench (for example `--limit 1000`) and look at the consumer's `joined` line. If it says "capped by net.core.rmem_max", stop and ask the user to run `wsl -u root sysctl -w net.core.rmem_max=268435456`, then rerun the check. Do not take measurements with a capped buffer.

- [ ] **Step 1: Full-file correctness run**

Warm the page cache, then replay the whole day:
```
wsl -e bash -lc 'cat ~/itch/01302019.NASDAQ_ITCH50 > /dev/null; cd /mnt/c/market && bash tools/feed_bench.sh ~/itch/01302019.NASDAQ_ITCH50 --rate 5e6'
```
Expected (about 75 s): `messages 368366634`, `gaps 0`, `kernel drops 0`, `live orders 0`, `peak 1742866`, all hard violations 0, and **top-of-book hash `fea88aae34305502`**, the same value `book_replay` gets from the file. A matching hash shows that every message arrived exactly once and in order through the network path. If drops occur at 5e6, rerun with `--rate 2e6` and record the rate that first produces drops.

- [ ] **Step 2: Rate sweep**

For each `R` in `1e6 2e6 5e6 10e6 0` (0 = unlimited), run:
```
wsl -e bash -lc 'cd /mnt/c/market && bash tools/feed_bench.sh ~/itch/01302019.NASDAQ_ITCH50 --rate R --limit 50000000'
```
Record for each run: the achieved M msgs/s, total p50/p99/p99.9/max, queue p99, and kernel drops/gaps. Rows where the sender itself is rate-limited or saturated report nonzero `rate-limited` counts on the replayer's `send lateness us` line; record those too, so a saturated sender is not mistaken for a consumer limit. A nonzero exit is expected once drops start. That is the saturation point and belongs in the results. (`--limit` stops early, so the hash will not match the full-day value. That is expected.)

- [ ] **Step 3: Real-time market open**

Run the open twice, once per batching window:
```
wsl -e bash -lc 'cd /mnt/c/market && bash tools/feed_bench.sh ~/itch/01302019.NASDAQ_ITCH50 --speed 1 --from 09:29:30 --to 09:35:00 --batch-ns 0'
wsl -e bash -lc 'cd /mnt/c/market && bash tools/feed_bench.sh ~/itch/01302019.NASDAQ_ITCH50 --speed 1 --from 09:29:30 --to 09:35:00 --batch-ns 10000'
```
`--from` now propagates to the consumer automatically, so no `CONSUMER_ARGS` is needed. Each takes about 6 minutes: catch-up at 5M/s, then 5.5 minutes in real time. Record the total/queue/book/svc lines and the replayer's whole `send lateness us` line (including re-bases and rate-limited). The 10 us run is the headline, because there the sender holds its schedule. The `--batch-ns 0` run is the sender-bound worst case; report it with its re-base count. If the sender fell behind (re-bases, high lateness), say so next to those results.

- [ ] **Step 4: Save raw output**

Keep each run's console output in a scratch file outside the repo, for Task 10.

---

### Task 10: Document Stage 3

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Update the stage table and layout**

In `README.md`, change the Stage 3 row to:
```
| 3 | UDP multicast replayer + real-time consumer, tick-to-book latency | done |
```
Add to the Layout block (keep the alignment):
```
include/net/mold.hpp          MoldUDP64 header, packetizer (file slices -> packets)
include/net/seq_tracker.hpp   session lock, gap / duplicate / overlap detection
include/net/feed_handler.hpp  socket-free packet -> books step, shared by tests and consumer
include/net/udp.hpp           multicast sockets, recvmmsg batch with kernel timestamps
include/net/os.hpp            clocks and CPU pinning
include/stats/histogram.hpp   fixed-size log-linear latency histogram
tools/feed_replayer.cpp       file -> paced MoldUDP64 multicast (zero-copy sendmsg)
tools/feed_consumer.cpp       multicast -> books, tick-to-book latency report
tools/feed_bench.sh           runs consumer + replayer together
tests/test_net.cpp            framing, sequencing, drop/dup/overlap through the books
tests/test_udp.cpp            loopback multicast smoke test (real sockets)
```
Add to "Build and run":
```bash
bash tools/feed_bench.sh ~/itch/01302019.NASDAQ_ITCH50 --rate 5e6       # full day over multicast
CONSUMER_ARGS="--from 09:29:30" bash tools/feed_bench.sh ~/itch/01302019.NASDAQ_ITCH50 \
    --speed 1 --from 09:29:30 --to 09:35:00                           # real-time market open
```
and the note: "Raise the socket buffer cap first: `sudo sysctl -w net.core.rmem_max=268435456`."

- [ ] **Step 2: Add "Design notes (stage 3)"**

Add these bullets after the stage 2 design notes:
- **MoldUDP64 = file slices.** Mold message blocks use the same framing as the sample file, so a packet is a header iovec plus a pointer into the mmap. The consumer passes the payload to the unchanged stage 1 parser.
- **Tick-to-book is measured from the kernel receive timestamp** (`SO_TIMESTAMPNS`). That is the closest software stand-in for a NIC hardware timestamp, and it is split into socket-queue time and parse+book time.
- **Losses are counted, never repaired.** Sequence gaps, the kernel's own drop counter (`SO_RXQ_OVFL`), malformed packets, and book invariants are all reported. Any of them fails the run. Recovery (a Mold request server) is out of scope.
- **Socket-free core.** `FeedHandler` holds all per-packet logic. Unit tests drive it with dropped, duplicated and overlapping packets and compare the resulting top-of-book hash to a direct parse.
- **Pacing.** A token bucket enforces `--rate` for catch-up and rate mode, with a `--max-burst-us` bound on how much the sender may catch up after a stall. With `--speed`, paced packets follow the ITCH timestamps only (bundled within `--batch-ns`), and the schedule re-bases after a packet is 1 ms late, so later bursts keep their real spacing.
- **Losses and clock steps.** Mold sequence gaps, including the end-of-session tail check, fail the run. The kernel drop counter is reported, but it is a lagging snapshot. Latency samples where the clock stepped are excluded and counted.
- **Caveat on Stage 2 timings.** The prefault fix means the `book_replay` timings in the README may have included page faults, since prefault was optimized away before. This is noted, not re-measured.

- [ ] **Step 3: Add results**

Under Results, add a "### Multicast feed" subsection with the data from Task 9:
1. Full-day run: messages, rate, gaps/drops, and the top-of-book hash (it should read `fea88aae34305502`, matching `book_replay`).
2. A rate-sweep table with columns `Replay rate | Achieved | total p50 | p99 | p99.9 | max | queue p99 | drops`, one row per rate from Task 9 Step 2.
3. The real-time 09:29:30–09:35:00 result: the histogram lines for both `--batch-ns` runs and the replayer's whole `send lateness us` line (re-bases, rate-limited).
4. Caveats: WSL2 is a VM, kernel UDP on loopback (no NIC), sender and receiver share the machine, realtime clock, and whether `rmem_max` was raised.

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "Document stage 3 multicast feed and latency results"
```
