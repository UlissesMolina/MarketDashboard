#pragma once

// Offline evaluation of signals against what the price did next.
//
// A sample is taken at a top-of-book change and gets two labels:
//   next_move  sign of the next change in the mid (+1 up, -1 down)
//   fwd_bps    mid return over a fixed horizon, in basis points
// Samples whose labels never resolve (end of data) are left unlabeled and
// ignored by evaluate().

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace signals {

constexpr std::size_t kNumSignals = 4;
constexpr std::array<const char*, kNumSignals> kSignalNames = {"imbalance", "microprice", "ofi", "trade_flow"};

struct Sample {
    std::uint64_t ts;
    double mid;
    std::array<double, kNumSignals> x;
    int next_move;   // 0 until resolved
    double fwd_bps;  // NaN until resolved; labeled() needs it finite

    [[nodiscard]] bool labeled() const noexcept { return next_move != 0 && std::isfinite(fwd_bps); }
};

// Labels one symbol's samples. Feed it every two-sided top-of-book change of
// that symbol (on_change), and add() a sample right after the change it was
// taken at. If the touch changes but is no longer two-sided, call drop_at(ts)
// instead. Timestamps must not decrease. Several messages of one event (an
// order that sweeps many resting orders) share a timestamp and each changes the
// touch, so the caller samples after each; only the sample after the last of
// them is a real prediction, and earlier ones at the same timestamp are dropped
// (their labels would be set by the rest of the same, already-decided event).
// That is why drop_at matters: an event that ends one-sided must not leave an
// earlier sample of the same event to be labeled by the event's own outcome.
// add() is ignored until the first on_change has set a positive mid.
class Labeler {
public:
    explicit Labeler(std::uint64_t horizon_ns) : horizon_(horizon_ns) {}

    // Drops the still-unlabeled samples taken at ts.
    // Samples at or past both cursors are unresolved, so dropping them leaves the cursors valid.
    void drop_at(std::uint64_t ts) {
        while (s_.size() > std::max(move_from_, horizon_from_) && s_.back().ts == ts) s_.pop_back();
    }

    void on_change(std::uint64_t ts, double new_mid) {
        // Several messages of one event share a timestamp; only the sample taken after the last of them is a real prediction.
        drop_at(ts);
        // A change after t + horizon means the mid at t + horizon was the current one.
        while (horizon_from_ < s_.size() && s_[horizon_from_].ts + horizon_ < ts) {
            Sample& s = s_[horizon_from_++];
            s.fwd_bps = (mid_ - s.mid) / s.mid * 1e4;
        }
        if (new_mid != mid_) {
            // Every sample still waiting was taken at the current mid.
            for (; move_from_ < s_.size(); ++move_from_) s_[move_from_].next_move = new_mid > mid_ ? 1 : -1;
            mid_ = new_mid;
        }
    }

    void add(std::uint64_t ts, const std::array<double, kNumSignals>& x) {
        if (mid_ <= 0) return;  // no mid yet: nothing to label against
        s_.push_back({ts, mid_, x, 0, std::numeric_limits<double>::quiet_NaN()});
    }

    [[nodiscard]] const std::vector<Sample>& samples() const noexcept { return s_; }

private:
    std::uint64_t horizon_;
    std::vector<Sample> s_;
    std::size_t horizon_from_ = 0;  // first sample without fwd_bps
    std::size_t move_from_ = 0;     // first sample without next_move
    double mid_ = 0;
};

struct Report {
    std::size_t n = 0;                     // labeled samples, from symbols with at least 10 of them
    std::size_t hits_n = 0;                // labeled samples with a nonzero signal
    double hit_rate = 0;                   // sign(signal) == next_move, over hits_n
    double corr = 0;                       // signal (standardized per symbol) vs fwd_bps
    std::array<double, 10> decile_bps{};   // mean fwd_bps by per-symbol decile of the signal
    std::array<std::size_t, 10> decile_n{};
};

// Scores signal k over several symbols. Deciles and standardization are done
// per symbol, so signals measured in shares pool across symbols of different
// size. Ties (a signal that is often exactly 0) are split arbitrarily between
// adjacent deciles.
inline Report evaluate(const std::vector<const std::vector<Sample>*>& per_symbol, std::size_t k) {
    Report r;
    std::size_t hits = 0;
    std::array<double, 10> sum{};
    double szy = 0, szz = 0, sy = 0, syy = 0;
    std::size_t nc = 0;  // samples contributing to corr
    for (const std::vector<Sample>* v : per_symbol) {
        std::vector<const Sample*> s;
        for (const Sample& x : *v)
            if (x.labeled()) s.push_back(&x);
        if (s.size() < 10) continue;
        double m = 0;
        for (const Sample* p : s) m += p->x[k];
        m /= double(s.size());
        double var = 0;
        for (const Sample* p : s) var += (p->x[k] - m) * (p->x[k] - m);
        const double sd = std::sqrt(var / double(s.size()));
        std::sort(s.begin(), s.end(), [k](const Sample* a, const Sample* b) { return a->x[k] < b->x[k]; });
        for (std::size_t i = 0; i < s.size(); ++i) {
            const double x = s[i]->x[k], y = s[i]->fwd_bps;
            ++r.n;
            if (x != 0) {
                ++r.hits_n;
                hits += (x > 0) == (s[i]->next_move > 0);
            }
            const std::size_t d = i * 10 / s.size();
            sum[d] += y;
            ++r.decile_n[d];
            if (sd > 0) {
                const double z = (x - m) / sd;
                szy += z * y;
                szz += z * z;
                sy += y;
                syy += y * y;
                ++nc;
            }
        }
    }
    r.hit_rate = r.hits_n ? double(hits) / double(r.hits_n) : 0.0;
    for (std::size_t d = 0; d < 10; ++d) r.decile_bps[d] = r.decile_n[d] ? sum[d] / double(r.decile_n[d]) : 0.0;
    if (nc > 0) {
        // z has mean 0 within each symbol, so cov(z, y) = E[z y].
        const double n = double(nc);
        const double vy = syy / n - (sy / n) * (sy / n);
        if (szz > 0 && vy > 0) r.corr = (szy / n) / std::sqrt(szz / n * vy);
    }
    return r;
}

}  // namespace signals
