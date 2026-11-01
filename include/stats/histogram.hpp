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
