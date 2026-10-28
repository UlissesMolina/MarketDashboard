#pragma once

// Handler that counts messages per type and folds the decoded fields into a
// checksum. itch_naive.cpp computes the same checksum with an independent
// decoder, so matching output cross-checks the zero-copy layouts on real data.
//
// Checksum = sum over all messages of timestamp + the fields listed in fields().

#include <array>
#include <cstdint>

#include "itch/messages.hpp"

namespace tools {

using namespace itch;

inline std::uint64_t fields(const AddOrder& m) { return m.order_ref + m.shares + m.price; }
inline std::uint64_t fields(const AddOrderMpid& m) { return m.order_ref + m.shares + m.price; }
inline std::uint64_t fields(const OrderExecuted& m) {
    return m.order_ref + m.executed_shares + m.match_number;
}
inline std::uint64_t fields(const OrderExecutedWithPrice& m) {
    return m.order_ref + m.executed_shares + m.match_number + m.execution_price;
}
inline std::uint64_t fields(const OrderCancel& m) { return m.order_ref + m.cancelled_shares; }
inline std::uint64_t fields(const OrderDelete& m) { return m.order_ref; }
inline std::uint64_t fields(const OrderReplace& m) {
    return m.original_order_ref + m.new_order_ref + m.shares + m.price;
}
inline std::uint64_t fields(const Trade& m) {
    return m.order_ref + m.shares + m.price + m.match_number;
}
inline std::uint64_t fields(const CrossTrade& m) {
    return m.shares + m.cross_price + m.match_number;
}
template <class M>
std::uint64_t fields(const M&) {
    return 0;
}

struct StatsHandler {
    std::array<std::uint64_t, 256> counts{};
    std::uint64_t checksum = 0;
    std::uint64_t unknown = 0;
    std::uint64_t last_ts = 0;
    std::uint64_t ts_regressions = 0;  // timestamp lower than the previous message's

    template <class M>
    void on(const M& m) {
        ++counts[static_cast<unsigned char>(m.hdr.type)];
        const std::uint64_t ts = m.hdr.timestamp;
        ts_regressions += ts < last_ts;
        last_ts = ts;
        checksum += ts + fields(m);
    }

    void on_unknown(char type, const unsigned char*, std::uint16_t) {
        ++counts[static_cast<unsigned char>(type)];
        ++unknown;
    }
};

}  // namespace tools
