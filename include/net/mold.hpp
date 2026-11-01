#pragma once

// MoldUDP64, Nasdaq's UDP framing for ITCH.
//
// A packet is a 20-byte header followed by `count` message blocks, each a 2-byte
// big-endian length and then the message. That block layout is exactly the
// framing of the ITCH sample files, so a packet's payload is a contiguous slice
// of the file: the replayer sends it straight out of the mmap and the consumer
// hands it to itch::parse unchanged.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "itch/endian.hpp"
#include "itch/messages.hpp"

namespace net {

constexpr std::size_t kSessionLen = 10;
constexpr std::uint16_t kHeartbeat = 0;
constexpr std::uint16_t kEndOfSession = 0xFFFF;
// Message-block bytes per packet. 1500 MTU - 20 IP - 8 UDP - 20 Mold = 1452.
constexpr std::size_t kMaxPayload = 1452;
constexpr std::size_t kDefaultMaxPayload = 1400;

struct MoldHeader {
    char session[kSessionLen];  // space-padded
    itch::be64 sequence;        // sequence number of the first message; a session starts at 1
    itch::be16 count;           // messages in the packet; 0 = heartbeat, 0xFFFF = end of session
};
static_assert(sizeof(MoldHeader) == 20 && alignof(MoldHeader) == 1);

inline void put_be(unsigned char* p, std::uint64_t v, int n) noexcept {
    for (int i = n - 1; i >= 0; --i) *p++ = static_cast<unsigned char>(v >> (8 * i));
}

[[nodiscard]] inline MoldHeader make_mold_header(std::string_view session, std::uint64_t seq,
                                                 std::uint16_t count) noexcept {
    MoldHeader h{};
    for (std::size_t i = 0; i < kSessionLen; ++i) h.session[i] = i < session.size() ? session[i] : ' ';
    put_be(h.sequence.bytes, seq, 8);
    put_be(h.count.bytes, count, 2);
    return h;
}

[[nodiscard]] inline std::string_view session_of(const MoldHeader& h) noexcept {
    return {h.session, kSessionLen};
}

// Skips n message blocks. Returns nullptr if they run past `end`.
[[nodiscard]] inline const unsigned char* skip_messages(const unsigned char* p, const unsigned char* end,
                                                        std::uint64_t n) noexcept {
    for (; n > 0; --n) {
        if (end - p < 2) return nullptr;
        const std::size_t len = itch::load_be16(p);
        if (static_cast<std::size_t>(end - p) - 2 < len) return nullptr;
        p += 2 + len;
    }
    return p;
}

// One packet's worth of message blocks.
struct Span {
    const unsigned char* begin;  // first message block
    std::size_t bytes;           // message-block bytes
    std::uint16_t count;         // messages
    std::uint64_t first_ts;      // ITCH timestamp of the first message
};

// Takes whole message blocks from [p, end) for one packet: as many as fit in
// max_payload bytes, at most max_count, and only while each message's timestamp
// is within max_span_ns of the first one. A paced replay passes 0 so it never
// holds a message back to batch it with a later event. Always takes one message
// if a complete one is available. Returns count == 0 at end of input or when the
// framing at p is truncated.
[[nodiscard]] inline Span next_span(const unsigned char* p, const unsigned char* end, std::size_t max_payload,
                                    std::uint64_t max_span_ns, std::uint64_t max_count) noexcept {
    Span s{p, 0, 0, 0};
    const unsigned char* q = p;
    while (s.count < max_count && s.count < kEndOfSession - 1 && end - q >= 2) {
        const std::size_t len = itch::load_be16(q);
        if (len < sizeof(itch::Header) || static_cast<std::size_t>(end - q) - 2 < len) break;
        const std::uint64_t ts = reinterpret_cast<const itch::Header*>(q + 2)->timestamp;
        if (s.count == 0) {
            s.first_ts = ts;
        } else if (s.bytes + 2 + len > max_payload || ts - s.first_ts > max_span_ns) {
            break;
        }
        s.bytes += 2 + len;
        ++s.count;
        q += 2 + len;
    }
    return s;
}

}  // namespace net
