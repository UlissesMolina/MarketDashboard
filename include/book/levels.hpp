#pragma once

// Price-level storage for one side of one book, aggregated by price (L2).
// Two interchangeable policies so they can be benchmarked on the same feed:
//
//   VectorLevels  sorted std::vector with the best price at the back. Activity
//                 clusters at the touch, so the linear scan from the back and the
//                 insert/erase shift both touch only a few elements.
//   MapLevels     std::map keyed by price, best first. The textbook baseline.
//
// IsBid selects the ordering: for bids higher is better, for asks lower is better.

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace book {

struct Level {
    std::uint32_t price;
    std::uint32_t orders;
    std::uint64_t shares;
};

template <bool IsBid>
constexpr bool better(std::uint32_t a, std::uint32_t b) noexcept {
    if constexpr (IsBid) return a > b;
    else return a < b;
}

template <bool IsBid>
class VectorLevels {
public:
    void add(std::uint32_t price, std::uint32_t shares) {
        // Scan from the best end to the first level not better than `price`.
        std::size_t i = levels_.size();
        while (i > 0 && better<IsBid>(levels_[i - 1].price, price)) --i;
        if (i > 0 && levels_[i - 1].price == price) {
            Level& l = levels_[i - 1];
            ++l.orders;
            l.shares += shares;
            return;
        }
        levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(i), Level{price, 1, shares});
    }

    // Removes `shares` at `price`; `order_gone` drops the order count too.
    // Returns false if the level is missing or would go negative.
    bool reduce(std::uint32_t price, std::uint32_t shares, bool order_gone) {
        std::size_t i = levels_.size();
        while (i > 0 && better<IsBid>(levels_[i - 1].price, price)) --i;
        if (i == 0 || levels_[i - 1].price != price) return false;
        Level& l = levels_[i - 1];
        if (l.shares < shares || (order_gone && l.orders == 0)) return false;
        l.shares -= shares;
        l.orders -= order_gone;
        if (l.orders == 0) levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(i - 1));
        return true;
    }

    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }
    [[nodiscard]] const Level& best() const noexcept { return levels_.back(); }
    [[nodiscard]] std::size_t depth() const noexcept { return levels_.size(); }

    template <class F>
    void for_each(F&& f) const {  // best first
        for (auto it = levels_.rbegin(); it != levels_.rend(); ++it) f(*it);
    }

private:
    std::vector<Level> levels_;  // worst ... best
};

template <bool IsBid>
class MapLevels {
    using Compare = std::conditional_t<IsBid, std::greater<std::uint32_t>, std::less<std::uint32_t>>;

public:
    void add(std::uint32_t price, std::uint32_t shares) {
        auto [it, inserted] = levels_.try_emplace(price, Level{price, 0, 0});
        ++it->second.orders;
        it->second.shares += shares;
    }

    bool reduce(std::uint32_t price, std::uint32_t shares, bool order_gone) {
        auto it = levels_.find(price);
        if (it == levels_.end()) return false;
        Level& l = it->second;
        if (l.shares < shares || (order_gone && l.orders == 0)) return false;
        l.shares -= shares;
        l.orders -= order_gone;
        if (l.orders == 0) levels_.erase(it);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }
    [[nodiscard]] const Level& best() const noexcept { return levels_.begin()->second; }
    [[nodiscard]] std::size_t depth() const noexcept { return levels_.size(); }

    template <class F>
    void for_each(F&& f) const {
        for (const auto& [p, l] : levels_) f(l);
    }

private:
    std::map<std::uint32_t, Level, Compare> levels_;
};

}  // namespace book
