#pragma once

// Zero-copy ITCH 5.0 parser.
//
// Input is the "binary file" framing Nasdaq uses for its sample files: each
// message is preceded by a 2-byte big-endian length. The parser walks the buffer
// in place and hands the handler a const reference that points straight into it.
//
// A handler is any type with `on(const Msg&)` overloads for the messages it cares
// about; message types without a matching overload are skipped at compile time.
// An optional `on_unknown(char type, const unsigned char* msg, std::uint16_t len)`
// sees message types this parser does not know.

#include <cstddef>
#include <cstdint>

#include "itch/endian.hpp"
#include "itch/messages.hpp"

namespace itch {

enum class ParseError {
    none,
    truncated,   // buffer ends partway through a message
    bad_length,  // a known message type with the wrong length (corrupt framing)
};

struct ParseResult {
    std::uint64_t messages = 0;
    std::size_t offset = 0;  // bytes consumed; on error, start of the bad message
    ParseError error = ParseError::none;
};

namespace detail {

template <class Msg, class H>
[[gnu::always_inline]] inline bool deliver(const unsigned char* m, std::uint16_t len, H& h) {
    if (len != sizeof(Msg)) [[unlikely]]
        return false;
    if constexpr (requires(H& x, const Msg& msg) { x.on(msg); })
        h.on(*reinterpret_cast<const Msg*>(m));
    return true;
}

template <class H>
[[gnu::always_inline]] inline bool dispatch(const unsigned char* m, std::uint16_t len, H& h) {
    // Ordered roughly by frequency in a real trading day.
    switch (m[0]) {
        case 'A': return deliver<AddOrder>(m, len, h);
        case 'D': return deliver<OrderDelete>(m, len, h);
        case 'U': return deliver<OrderReplace>(m, len, h);
        case 'E': return deliver<OrderExecuted>(m, len, h);
        case 'X': return deliver<OrderCancel>(m, len, h);
        case 'I': return deliver<Noii>(m, len, h);
        case 'F': return deliver<AddOrderMpid>(m, len, h);
        case 'P': return deliver<Trade>(m, len, h);
        case 'L': return deliver<MarketParticipantPosition>(m, len, h);
        case 'C': return deliver<OrderExecutedWithPrice>(m, len, h);
        case 'Q': return deliver<CrossTrade>(m, len, h);
        case 'Y': return deliver<RegShoRestriction>(m, len, h);
        case 'H': return deliver<StockTradingAction>(m, len, h);
        case 'R': return deliver<StockDirectory>(m, len, h);
        case 'J': return deliver<LuldAuctionCollar>(m, len, h);
        case 'N': return deliver<RetailPriceImprovement>(m, len, h);
        case 'B': return deliver<BrokenTrade>(m, len, h);
        case 'S': return deliver<SystemEvent>(m, len, h);
        case 'K': return deliver<IpoQuotingPeriodUpdate>(m, len, h);
        case 'h': return deliver<OperationalHalt>(m, len, h);
        case 'V': return deliver<MwcbDeclineLevel>(m, len, h);
        case 'W': return deliver<MwcbStatus>(m, len, h);
        case 'O': return deliver<DirectListingPriceDiscovery>(m, len, h);
        default:
            if constexpr (requires(H& x) { x.on_unknown(char{}, m, len); })
                h.on_unknown(static_cast<char>(m[0]), m, len);
            return true;
    }
}

}  // namespace detail

template <class H>
ParseResult parse(const unsigned char* begin, const unsigned char* end, H& handler) {
    ParseResult r;
    const unsigned char* p = begin;
    while (end - p >= 2) {
        const std::uint16_t len = load_be16(p);
        if (len == 0) [[unlikely]] {
            r.error = ParseError::bad_length;
            break;
        }
        if (end - p - 2 < len) [[unlikely]] {
            r.error = ParseError::truncated;
            break;
        }
        if (!detail::dispatch(p + 2, len, handler)) [[unlikely]] {
            r.error = ParseError::bad_length;
            break;
        }
        p += 2 + len;
        ++r.messages;
    }
    if (r.error == ParseError::none && p != end) r.error = ParseError::truncated;
    r.offset = static_cast<std::size_t>(p - begin);
    return r;
}

}  // namespace itch
