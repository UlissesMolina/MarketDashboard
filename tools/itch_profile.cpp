// itch_profile: one pass over a file to measure the properties the order book
// design depends on: order-ref range and density, peak live orders, stock locate
// range, and per-symbol price ranges.
//
//   itch_profile <file>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "itch/mapped_file.hpp"
#include "itch/parser.hpp"

namespace {

struct Profiler {
    std::vector<std::uint64_t> live_bits;  // 1 bit per order ref
    std::uint64_t live = 0, peak_live = 0, peak_live_ts = 0;
    std::uint64_t min_ref = UINT64_MAX, max_ref = 0, add_count = 0;
    std::uint64_t non_monotonic_adds = 0, last_add_ref = 0;
    std::uint64_t unknown_ref_events = 0;
    std::uint16_t max_locate = 0;
    std::vector<std::uint32_t> min_px = std::vector<std::uint32_t>(65536, UINT32_MAX);
    std::vector<std::uint32_t> max_px = std::vector<std::uint32_t>(65536, 0);

    bool test(std::uint64_t r) const {
        return r / 64 < live_bits.size() && (live_bits[r / 64] >> (r % 64)) & 1;
    }
    void set(std::uint64_t r) {
        if (r / 64 >= live_bits.size()) live_bits.resize(std::max<std::size_t>(r / 64 + 1, live_bits.size() * 2));
        live_bits[r / 64] |= 1ULL << (r % 64);
    }
    void clear(std::uint64_t r) { live_bits[r / 64] &= ~(1ULL << (r % 64)); }

    void add(std::uint64_t ref, std::uint16_t locate, std::uint32_t px, std::uint64_t ts) {
        ++add_count;
        min_ref = std::min(min_ref, ref);
        max_ref = std::max(max_ref, ref);
        non_monotonic_adds += ref <= last_add_ref;
        last_add_ref = ref;
        max_locate = std::max(max_locate, locate);
        min_px[locate] = std::min(min_px[locate], px);
        max_px[locate] = std::max(max_px[locate], px);
        set(ref);
        if (++live > peak_live) {
            peak_live = live;
            peak_live_ts = ts;
        }
    }
    void remove(std::uint64_t ref) {
        if (!test(ref)) {
            ++unknown_ref_events;
            return;
        }
        clear(ref);
        --live;
    }

    void on(const itch::AddOrder& m) { add(m.order_ref, m.hdr.stock_locate, m.price, m.hdr.timestamp); }
    void on(const itch::AddOrderMpid& m) { add(m.order_ref, m.hdr.stock_locate, m.price, m.hdr.timestamp); }
    void on(const itch::OrderDelete& m) { remove(m.order_ref); }
    void on(const itch::OrderReplace& m) {
        remove(m.original_order_ref);
        add(m.new_order_ref, m.hdr.stock_locate, m.price, m.hdr.timestamp);
        --add_count;  // count only true adds
    }
    // Full executions/cancels are not tracked here (shares not modelled), so
    // peak_live is an upper bound; deletes and replaces dominate removals.
    void on(const itch::OrderExecuted& m) { if (!test(m.order_ref)) ++unknown_ref_events; }
    void on(const itch::OrderCancel& m) { if (!test(m.order_ref)) ++unknown_ref_events; }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <itch-file>\n", argv[0]);
        return 2;
    }
    itch::MappedFile file(argv[1]);
    Profiler p;
    p.live_bits.resize(1 << 20);
    const auto r = itch::parse(file.data(), file.end(), p);

    std::printf("messages            %llu (error %d)\n", (unsigned long long)r.messages, (int)r.error);
    std::printf("adds (A+F)          %llu\n", (unsigned long long)p.add_count);
    std::printf("order ref range     %llu .. %llu\n", (unsigned long long)p.min_ref, (unsigned long long)p.max_ref);
    std::printf("non-monotonic adds  %llu\n", (unsigned long long)p.non_monotonic_adds);
    std::printf("unknown-ref events  %llu\n", (unsigned long long)p.unknown_ref_events);
    std::printf("peak live (bound)   %llu at %.3f h\n", (unsigned long long)p.peak_live, p.peak_live_ts / 3.6e12);
    std::printf("still live at end   %llu\n", (unsigned long long)p.live);
    std::printf("max stock locate    %u\n", p.max_locate);

    // Price-range distribution in ticks of $0.01 (Price(4) units / 100).
    std::vector<std::uint64_t> ranges;
    std::uint32_t max_px_all = 0;
    for (int l = 0; l < 65536; ++l) {
        if (p.max_px[l] == 0) continue;
        ranges.push_back((p.max_px[l] - p.min_px[l]) / 100);
        max_px_all = std::max(max_px_all, p.max_px[l]);
    }
    std::sort(ranges.begin(), ranges.end());
    auto pct = [&](double q) { return ranges[std::min(ranges.size() - 1, std::size_t(q * ranges.size()))]; };
    std::printf("symbols with adds   %zu\n", ranges.size());
    std::printf("day price range in cents: p50 %llu  p90 %llu  p99 %llu  max %llu\n",
                (unsigned long long)pct(0.5), (unsigned long long)pct(0.9), (unsigned long long)pct(0.99),
                (unsigned long long)ranges.back());
    std::printf("max add price       $%.4f\n", max_px_all / 1e4);
}
