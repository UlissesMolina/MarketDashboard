#pragma once

// Keeps every symbol's signals current as a BookBuilder listener: top-of-book
// changes feed OFI, executions feed signed trade flow. Imbalance and microprice
// are read straight off the last top of book when a snapshot is taken.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "book/book_builder.hpp"
#include "signals/signals.hpp"

namespace signals {

struct Snapshot {
    double imbalance;  // [-1, 1]; 0 if not two-sided
    double micro_bps;  // (microprice - mid) / mid, basis points; 0 if not two-sided
    double ofi;        // decayed order-flow imbalance, shares
    double flow;       // decayed signed trade volume, shares
};

class SignalEngine {
public:
    static constexpr std::size_t kMaxLocate = 65536;

    // Two constructors rather than one explicit one with a default argument:
    // BookBuilder default-initializes its listener with `= {}`, which needs a
    // non-explicit default constructor.
    SignalEngine() : SignalEngine(1e9) {}
    explicit SignalEngine(double tau_ns) : tau_(tau_ns), states_(kMaxLocate) {}

    // --- BookBuilder listener interface --------------------------------------

    template <class Book>
    void on_book_update(std::uint16_t locate, std::uint64_t ts, const Book& b) {
        update_bbo(locate, ts, b.bbo());
    }

    void on_execution(std::uint16_t locate, std::uint64_t ts, char resting_side, std::uint32_t shares,
                      std::uint32_t /*price*/) {
        State& s = states_[locate];
        s.flow.add(ts, signed_volume(resting_side, shares), tau_);
        ++s.executions;
        last_ts_ = std::max(last_ts_, ts);
    }

    // ---------------------------------------------------------------------------

    // Records a symbol's top of book. Returns false if it equals the last one
    // (the update was deeper in the book), true if the touch changed.
    bool update_bbo(std::uint16_t locate, std::uint64_t ts, const book::Bbo& q) {
        last_ts_ = std::max(last_ts_, ts);
        State& s = states_[locate];
        if (q == s.bbo) return false;
        if (two_sided(s.bbo) && two_sided(q)) s.ofi.add(ts, ofi(s.bbo, q), tau_);
        s.bbo = q;
        ++s.bbo_changes;
        return true;
    }

    // Signals for one symbol as of ts (decayed sums are aged to ts).
    [[nodiscard]] Snapshot snapshot(std::uint16_t locate, std::uint64_t ts) const {
        const State& s = states_[locate];
        const double m = two_sided(s.bbo) ? mid(s.bbo) : 0.0;
        return {m > 0 ? imbalance(s.bbo) : 0.0, m > 0 ? (microprice(s.bbo) - m) / m * 1e4 : 0.0, s.ofi.at(ts, tau_),
                s.flow.at(ts, tau_)};
    }

    [[nodiscard]] const book::Bbo& bbo(std::uint16_t locate) const { return states_[locate].bbo; }
    [[nodiscard]] std::uint64_t bbo_changes(std::uint16_t locate) const { return states_[locate].bbo_changes; }
    [[nodiscard]] std::uint64_t executions(std::uint16_t locate) const { return states_[locate].executions; }
    [[nodiscard]] std::uint64_t last_ts() const noexcept { return last_ts_; }
    [[nodiscard]] double tau_ns() const noexcept { return tau_; }

private:
    struct State {
        book::Bbo bbo{};
        DecayingSum ofi;
        DecayingSum flow;
        std::uint64_t bbo_changes = 0;
        std::uint64_t executions = 0;
    };

    double tau_;
    std::vector<State> states_;
    std::uint64_t last_ts_ = 0;
};

}  // namespace signals
