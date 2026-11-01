#pragma once

// Microstructure signals computed from one symbol's top of book and trades.
// Prices are ITCH Price(4) integers ($0.0001 units); sizes are shares.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "book/book_builder.hpp"

namespace signals {

[[nodiscard]] inline bool two_sided(const book::Bbo& q) noexcept {
    return q.bid != 0 && q.ask != 0 && q.bid < q.ask;
}

[[nodiscard]] inline double mid(const book::Bbo& q) noexcept { return (double(q.bid) + double(q.ask)) / 2; }

// Size imbalance at the touch, in [-1, 1]: +1 when all displayed size is on the bid.
[[nodiscard]] inline double imbalance(const book::Bbo& q) noexcept {
    const double b = double(q.bid_shares), a = double(q.ask_shares);
    return b + a > 0 ? (b - a) / (b + a) : 0.0;
}

// Size-weighted mid: leans toward the side with less size, which the next trade
// is likelier to take out. Equals mid + spread / 2 * imbalance.
[[nodiscard]] inline double microprice(const book::Bbo& q) noexcept {
    const double b = double(q.bid_shares), a = double(q.ask_shares);
    if (b + a == 0) return mid(q);
    return (double(q.bid) * a + double(q.ask) * b) / (b + a);
}

// Order-flow imbalance of one top-of-book change (Cont, Kukanov & Stoikov 2014).
// Size arriving at or above the old best bid, or leaving at or below the old
// best ask, is buy pressure (positive); the mirror images are sell pressure.
// Both quotes should be two-sided.
[[nodiscard]] inline double ofi(const book::Bbo& prev, const book::Bbo& cur) noexcept {
    double e = 0;
    if (cur.bid >= prev.bid) e += double(cur.bid_shares);
    if (cur.bid <= prev.bid) e -= double(prev.bid_shares);
    if (cur.ask <= prev.ask) e -= double(cur.ask_shares);
    if (cur.ask >= prev.ask) e += double(prev.ask_shares);
    return e;
}

// Execution size signed by the aggressor: a trade against a resting buy order
// was sell-initiated, so it counts negative.
[[nodiscard]] inline double signed_volume(char resting_side, std::uint32_t shares) noexcept {
    return resting_side == 'B' ? -double(shares) : double(shares);
}

// A sum whose contents decay exponentially with time constant tau (ns of feed
// time), so recent flow dominates without keeping a window of events.
class DecayingSum {
public:
    void add(std::uint64_t ts, double x, double tau_ns) noexcept {
        value_ = at(ts, tau_ns) + x;
        ts_ = std::max(ts_, ts);
    }

    // The sum as of ts, without changing it. Times before the last add return
    // the value at the last add.
    [[nodiscard]] double at(std::uint64_t ts, double tau_ns) const noexcept {
        return ts > ts_ ? value_ * std::exp(-double(ts - ts_) / tau_ns) : value_;
    }

private:
    double value_ = 0;
    std::uint64_t ts_ = 0;
};

}  // namespace signals
