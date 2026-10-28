#pragma once

// Open-addressing hash map from order reference number to live order state.
// Linear probing with backward-shift deletion (no tombstones), so probe chains
// stay short across hundreds of millions of insert/erase cycles. Ref 0 marks an
// empty slot; ITCH order refs start above 0.
//
// Pointers returned by find/insert are invalidated by any later insert or erase.

#include <cstdint>
#include <vector>

namespace book {

struct Order {
    std::uint64_t ref;
    std::uint32_t shares;  // remaining displayed shares
    std::uint32_t price;   // Price(4)
    std::uint16_t locate;
    char side;  // 'B' or 'S'
};

class OrderMap {
public:
    explicit OrderMap(unsigned log2_capacity = 24) { resize(log2_capacity); }

    [[nodiscard]] Order* find(std::uint64_t ref) noexcept {
        for (std::size_t i = home(ref);; i = (i + 1) & mask_) {
            Order& s = slots_[i];
            if (s.ref == ref) return &s;
            if (s.ref == 0) return nullptr;
        }
    }

    // Returns the slot for a new order, or nullptr if `ref` is already present.
    // The caller fills in the remaining fields.
    [[nodiscard]] Order* insert(std::uint64_t ref) {
        if ((size_ + 1) * 10 > slots_.size() * 7) [[unlikely]]
            grow();
        for (std::size_t i = home(ref);; i = (i + 1) & mask_) {
            Order& s = slots_[i];
            if (s.ref == ref) return nullptr;
            if (s.ref == 0) {
                s.ref = ref;
                ++size_;
                return &s;
            }
        }
    }

    void erase(Order* slot) noexcept {
        std::size_t i = static_cast<std::size_t>(slot - slots_.data());
        for (std::size_t j = (i + 1) & mask_; slots_[j].ref != 0; j = (j + 1) & mask_) {
            const std::size_t k = home(slots_[j].ref);
            // Slot j may move back to i only if its home is not cyclically in (i, j].
            const bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (stays) continue;
            slots_[i] = slots_[j];
            i = j;
        }
        slots_[i].ref = 0;
        --size_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

    template <class F>
    void for_each(F&& f) const {
        for (const Order& s : slots_)
            if (s.ref != 0) f(s);
    }

private:
    [[nodiscard]] std::size_t home(std::uint64_t ref) const noexcept {
        return static_cast<std::size_t>((ref * 0x9E3779B97F4A7C15ULL) >> shift_);
    }

    void resize(unsigned log2_capacity) {
        slots_.assign(std::size_t{1} << log2_capacity, Order{});
        mask_ = slots_.size() - 1;
        shift_ = 64 - log2_capacity;
        log2_ = log2_capacity;
        size_ = 0;
    }

    void grow() {
        std::vector<Order> old = std::move(slots_);
        resize(log2_ + 1);
        for (const Order& s : old) {
            if (s.ref == 0) continue;
            std::size_t i = home(s.ref);
            while (slots_[i].ref != 0) i = (i + 1) & mask_;
            slots_[i] = s;
            ++size_;
        }
    }

    std::vector<Order> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
    unsigned shift_ = 0;
    unsigned log2_ = 0;
};

}  // namespace book
