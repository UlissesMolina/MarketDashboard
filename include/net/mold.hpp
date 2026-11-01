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

}  // namespace net
