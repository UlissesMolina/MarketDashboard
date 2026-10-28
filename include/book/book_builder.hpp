#pragma once

// Builds L2 order books for every symbol from ITCH order messages, and checks
// feed invariants as it goes. Use it as an itch::parse handler.
//
// Invariants (each violation is counted, never silently ignored):
//   - every execute / cancel / delete / replace refers to a live order
//   - executed or cancelled shares never exceed the order's remaining shares
//   - the message's stock locate matches the order's
//   - an add never reuses a live order ref
//   - level aggregates stay consistent with the orders (checked in audit())
//   - the book is never crossed or locked while the market is open and the
//     symbol is trading (crossed books are legal pre-open and during halts)

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "book/levels.hpp"
#include "book/order_map.hpp"
#include "itch/messages.hpp"

namespace book {

struct Violations {
    std::uint64_t unknown_ref = 0;
    std::uint64_t overfill = 0;
    std::uint64_t locate_mismatch = 0;
    std::uint64_t duplicate_add = 0;
    std::uint64_t level_mismatch = 0;  // reduce() found no level or too few shares
    std::uint64_t crossed_open = 0;    // crossed while market open and symbol trading
    std::uint64_t locked_open = 0;
    std::uint64_t crossed_other = 0;   // pre/post market or halted: informational

    [[nodiscard]] std::uint64_t hard() const noexcept {
        return unknown_ref + overfill + locate_mismatch + duplicate_add + level_mismatch +
               crossed_open + locked_open;
    }
};

struct Bbo {
    std::uint32_t bid = 0, ask = 0;  // 0 = side empty
    std::uint64_t bid_shares = 0, ask_shares = 0;
};

template <template <bool> class Levels>
struct Book {
    Levels<true> bids;
    Levels<false> asks;
    char trading_state = ' ';  // from 'H': T = trading, H/P/Q = halted/paused/quoting

    [[nodiscard]] Bbo bbo() const noexcept {
        Bbo b;
        if (!bids.empty()) b.bid = bids.best().price, b.bid_shares = bids.best().shares;
        if (!asks.empty()) b.ask = asks.best().price, b.ask_shares = asks.best().shares;
        return b;
    }
};

template <template <bool> class Levels>
class BookBuilder {
public:
    static constexpr std::size_t kMaxLocate = 65536;

    // audit_times: nanoseconds since midnight at which to run audit() mid-feed
    // (on the first add at or after each time). Their cost is reported separately.
    explicit BookBuilder(unsigned order_map_log2 = 22, std::vector<std::uint64_t> audit_times = {})
        : orders_(order_map_log2), books_(kMaxLocate), symbols_(kMaxLocate), audit_times_(std::move(audit_times)) {
        std::sort(audit_times_.begin(), audit_times_.end());
    }

    // --- itch::parse handler interface ---------------------------------------

    void on(const itch::SystemEvent& m) {
        if (m.event_code == 'Q') market_open_ = true;
        if (m.event_code == 'M') {
            market_open_ = false;
            close_bbo_.resize(kMaxLocate);
            for (std::size_t l = 0; l < kMaxLocate; ++l) close_bbo_[l] = books_[l].bbo();
        }
    }
    void on(const itch::StockDirectory& m) { symbols_[m.hdr.stock_locate] = std::string(itch::trim(m.stock)); }
    void on(const itch::StockTradingAction& m) { books_[m.hdr.stock_locate].trading_state = m.trading_state; }

    void on(const itch::AddOrder& m) {
        maybe_audit(m.hdr.timestamp);
        add(m.hdr.stock_locate, m.order_ref, m.side, m.shares, m.price);
    }
    void on(const itch::AddOrderMpid& m) {
        maybe_audit(m.hdr.timestamp);
        add(m.hdr.stock_locate, m.order_ref, m.side, m.shares, m.price);
    }
    void on(const itch::OrderExecuted& m) { reduce(m.hdr.stock_locate, m.order_ref, m.executed_shares, false); }
    void on(const itch::OrderExecutedWithPrice& m) {
        reduce(m.hdr.stock_locate, m.order_ref, m.executed_shares, false);
    }
    void on(const itch::OrderCancel& m) { reduce(m.hdr.stock_locate, m.order_ref, m.cancelled_shares, false); }
    void on(const itch::OrderDelete& m) { reduce(m.hdr.stock_locate, m.order_ref, 0, true); }

    void on(const itch::OrderReplace& m) {
        const std::uint16_t locate = m.hdr.stock_locate;
        Order* o = find(locate, m.original_order_ref);
        if (!o) return;
        const char side = o->side;
        remove(*o);
        add(locate, m.new_order_ref, side, m.shares, m.price);
    }

    void on(const itch::CrossTrade& m) {
        if (m.cross_type == 'C') closing_crosses_.push_back({m.hdr.stock_locate, m.cross_price, m.shares});
    }

    // --- inspection ----------------------------------------------------------

    struct AuditResult {
        std::uint64_t timestamp;
        std::size_t live_orders;
        std::uint64_t mismatches;
    };

    struct ClosingCross {
        std::uint16_t locate;
        std::uint32_t price;
        std::uint64_t shares;
    };

    [[nodiscard]] const Violations& violations() const noexcept { return v_; }
    [[nodiscard]] const Book<Levels>& book(std::uint16_t locate) const { return books_[locate]; }
    // Mutable access so tests can corrupt a book and prove audit() notices.
    [[nodiscard]] Book<Levels>& book_for_test(std::uint16_t locate) { return books_[locate]; }
    [[nodiscard]] const std::string& symbol(std::uint16_t locate) const { return symbols_[locate]; }
    [[nodiscard]] std::size_t live_orders() const noexcept { return orders_.size(); }
    [[nodiscard]] std::size_t peak_live_orders() const noexcept { return peak_live_; }
    [[nodiscard]] std::uint64_t tob_hash() const noexcept { return tob_hash_; }
    [[nodiscard]] const std::vector<AuditResult>& audits() const noexcept { return audits_; }
    [[nodiscard]] double audit_seconds() const noexcept { return audit_seconds_; }
    [[nodiscard]] const std::vector<ClosingCross>& closing_crosses() const noexcept { return closing_crosses_; }
    // BBO snapshot taken at the end-of-market-hours system event ('M').
    [[nodiscard]] const std::vector<Bbo>& close_bbo() const noexcept { return close_bbo_; }

    [[nodiscard]] int locate_of(std::string_view sym) const {
        for (std::size_t l = 0; l < kMaxLocate; ++l)
            if (symbols_[l] == sym) return static_cast<int>(l);
        return -1;
    }

    // Recomputes every level from the live orders and compares with the
    // incrementally maintained books. Returns the number of mismatched levels.
    std::uint64_t audit() {
        using Key = std::tuple<std::uint16_t, char, std::uint32_t, std::uint64_t, std::uint32_t>;
        std::vector<std::tuple<std::uint16_t, char, std::uint32_t, std::uint32_t>> raw;
        raw.reserve(orders_.size());
        orders_.for_each([&](const Order& o) { raw.emplace_back(o.locate, o.side, o.price, o.shares); });
        std::sort(raw.begin(), raw.end());

        std::vector<Key> expected;
        for (const auto& [loc, side, px, sh] : raw) {
            if (!expected.empty()) {
                auto& [el, es, ep, eshares, eorders] = expected.back();
                if (el == loc && es == side && ep == px) {
                    eshares += sh;
                    ++eorders;
                    continue;
                }
            }
            expected.emplace_back(loc, side, px, sh, 1);
        }

        std::vector<Key> actual;
        for (std::size_t l = 0; l < kMaxLocate; ++l) {
            const auto loc = static_cast<std::uint16_t>(l);
            books_[l].bids.for_each([&](const Level& lv) { actual.emplace_back(loc, 'B', lv.price, lv.shares, lv.orders); });
            books_[l].asks.for_each([&](const Level& lv) { actual.emplace_back(loc, 'S', lv.price, lv.shares, lv.orders); });
        }
        std::sort(actual.begin(), actual.end());

        std::vector<Key> diff;
        std::set_symmetric_difference(expected.begin(), expected.end(), actual.begin(), actual.end(),
                                      std::back_inserter(diff));
        return diff.size();
    }

private:
    void maybe_audit(std::uint64_t ts) {
        if (next_audit_ < audit_times_.size() && ts >= audit_times_[next_audit_]) [[unlikely]] {
            const auto t0 = std::chrono::steady_clock::now();
            audits_.push_back({ts, orders_.size(), audit()});
            audit_seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            ++next_audit_;
        }
    }

    Order* find(std::uint16_t locate, std::uint64_t ref) {
        Order* o = orders_.find(ref);
        if (!o) [[unlikely]] {
            ++v_.unknown_ref;
            return nullptr;
        }
        if (o->locate != locate) [[unlikely]] {
            ++v_.locate_mismatch;
            return nullptr;
        }
        return o;
    }

    void add(std::uint16_t locate, std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t price) {
        Order* o = orders_.insert(ref);
        if (!o) [[unlikely]] {
            ++v_.duplicate_add;
            return;
        }
        o->shares = shares;
        o->price = price;
        o->locate = locate;
        o->side = side;
        peak_live_ = std::max(peak_live_, orders_.size());

        Book<Levels>& b = books_[locate];
        if (side == 'B') b.bids.add(price, shares);
        else b.asks.add(price, shares);
        check_cross(b);
        touch(locate, b);
    }

    // shares == 0 with whole == true removes the order's remaining shares.
    void reduce(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares, bool whole) {
        Order* o = find(locate, ref);
        if (!o) return;
        if (whole) {
            remove(*o);
        } else {
            if (shares > o->shares) [[unlikely]] {
                ++v_.overfill;
                shares = o->shares;
            }
            o->shares -= shares;
            const bool gone = o->shares == 0;
            reduce_level(locate, o->side, o->price, shares, gone);
            if (gone) orders_.erase(o);
        }
        touch(locate, books_[locate]);
    }

    void remove(Order& o) {
        reduce_level(o.locate, o.side, o.price, o.shares, true);
        orders_.erase(&o);
    }

    void reduce_level(std::uint16_t locate, char side, std::uint32_t price, std::uint32_t shares, bool gone) {
        Book<Levels>& b = books_[locate];
        const bool ok = side == 'B' ? b.bids.reduce(price, shares, gone) : b.asks.reduce(price, shares, gone);
        v_.level_mismatch += !ok;
    }

    void check_cross(const Book<Levels>& b) {
        if (b.bids.empty() || b.asks.empty()) return;
        const std::uint32_t bid = b.bids.best().price, ask = b.asks.best().price;
        if (bid < ask) [[likely]]
            return;
        if (market_open_ && b.trading_state == 'T') {
            if (bid > ask) ++v_.crossed_open;
            else ++v_.locked_open;
        } else {
            ++v_.crossed_other;
        }
    }

    // Running hash of the top of book after every update, so two level policies
    // can be compared message-by-message without storing their full history.
    void touch(std::uint16_t locate, const Book<Levels>& b) {
        const Bbo q = b.bbo();
        std::uint64_t h = locate;
        h = h * 0x100000001B3ULL ^ q.bid;
        h = h * 0x100000001B3ULL ^ q.ask;
        h = h * 0x100000001B3ULL ^ q.bid_shares;
        h = h * 0x100000001B3ULL ^ q.ask_shares;
        tob_hash_ = (tob_hash_ ^ h) * 0x9E3779B97F4A7C15ULL;
    }

    OrderMap orders_;
    std::vector<Book<Levels>> books_;
    std::vector<std::string> symbols_;
    std::vector<ClosingCross> closing_crosses_;
    std::vector<Bbo> close_bbo_;
    std::vector<std::uint64_t> audit_times_;
    std::size_t next_audit_ = 0;
    std::vector<AuditResult> audits_;
    double audit_seconds_ = 0;
    Violations v_;
    std::size_t peak_live_ = 0;
    std::uint64_t tob_hash_ = 0;
    bool market_open_ = false;
};

}  // namespace book
