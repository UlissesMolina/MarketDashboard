#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace itch {

static_assert(std::endian::native == std::endian::little,
              "decoders assume a little-endian host");

// An N-byte big-endian unsigned integer stored inline with alignment 1.
// Message structs are built from these so they overlay the raw feed bytes
// exactly: no padding, no copies, and decoding only happens on field access.
// Reads go through unsigned char, which is allowed to alias anything.
template <std::size_t N>
struct BigEndian {
    static_assert(N == 2 || N == 4 || N == 6 || N == 8);

    using value_type =
        std::conditional_t<N == 2, std::uint16_t,
                           std::conditional_t<N == 4, std::uint32_t, std::uint64_t>>;

    unsigned char bytes[N];

    [[nodiscard]] value_type get() const noexcept {
        if constexpr (N == 2) {
            std::uint16_t v;
            std::memcpy(&v, bytes, 2);
            return __builtin_bswap16(v);
        } else if constexpr (N == 4) {
            std::uint32_t v;
            std::memcpy(&v, bytes, 4);
            return __builtin_bswap32(v);
        } else if constexpr (N == 8) {
            std::uint64_t v;
            std::memcpy(&v, bytes, 8);
            return __builtin_bswap64(v);
        } else {
            // 48-bit timestamp: nanoseconds since midnight.
            std::uint16_t hi;
            std::uint32_t lo;
            std::memcpy(&hi, bytes, 2);
            std::memcpy(&lo, bytes + 2, 4);
            return (std::uint64_t{__builtin_bswap16(hi)} << 32) | __builtin_bswap32(lo);
        }
    }

    operator value_type() const noexcept { return get(); }
};

using be16 = BigEndian<2>;
using be32 = BigEndian<4>;
using be48 = BigEndian<6>;
using be64 = BigEndian<8>;

static_assert(sizeof(be48) == 6 && alignof(be48) == 1);

[[nodiscard]] inline std::uint16_t load_be16(const unsigned char* p) noexcept {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}

}  // namespace itch
