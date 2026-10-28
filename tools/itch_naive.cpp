// itch_naive: the "obvious first attempt" baseline. Reads the file through an
// ifstream, allocates a buffer per message, and decodes each field by hand into
// an owning struct. It shares no decoding code with include/itch, so a checksum
// that matches itch_stats is an independent check of the zero-copy layouts.
//
//   itch_naive <file>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::uint64_t be(const std::vector<char>& b, std::size_t off, std::size_t n) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) v = (v << 8) | static_cast<unsigned char>(b.at(off + i));
    return v;
}

struct Decoded {
    char type = 0;
    std::uint64_t timestamp = 0;
    std::uint64_t order_ref = 0, new_order_ref = 0;
    std::uint64_t shares = 0, price = 0, match = 0;
    std::string stock;
};

// Byte offsets from the type byte, as in the spec tables. The header is
// type(1) locate(2) tracking(2) timestamp(6) = 11 bytes.
bool decode(const std::vector<char>& b, Decoded& d) {
    d = {};
    d.type = b.at(0);
    d.timestamp = be(b, 5, 6);
    switch (d.type) {
        case 'A':
        case 'F':
            d.order_ref = be(b, 11, 8);
            d.shares = be(b, 20, 4);
            d.stock.assign(&b.at(24), 8);
            d.price = be(b, 32, 4);
            return true;
        case 'E':
            d.order_ref = be(b, 11, 8);
            d.shares = be(b, 19, 4);
            d.match = be(b, 23, 8);
            return true;
        case 'C':
            d.order_ref = be(b, 11, 8);
            d.shares = be(b, 19, 4);
            d.match = be(b, 23, 8);
            d.price = be(b, 32, 4);
            return true;
        case 'X':
            d.order_ref = be(b, 11, 8);
            d.shares = be(b, 19, 4);
            return true;
        case 'D':
            d.order_ref = be(b, 11, 8);
            return true;
        case 'U':
            d.order_ref = be(b, 11, 8);
            d.new_order_ref = be(b, 19, 8);
            d.shares = be(b, 27, 4);
            d.price = be(b, 31, 4);
            return true;
        case 'P':
            d.order_ref = be(b, 11, 8);
            d.shares = be(b, 20, 4);
            d.stock.assign(&b.at(24), 8);
            d.price = be(b, 32, 4);
            d.match = be(b, 36, 8);
            return true;
        case 'Q':
            d.shares = be(b, 11, 8);
            d.stock.assign(&b.at(19), 8);
            d.price = be(b, 27, 4);
            d.match = be(b, 31, 8);
            return true;
        case 'S': case 'R': case 'H': case 'Y': case 'L': case 'V': case 'W':
        case 'K': case 'J': case 'h': case 'B': case 'I': case 'N': case 'O':
            return true;
        default:
            return false;  // unknown type: not part of the checksum
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <itch-file>\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }

    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t messages = 0, bytes = 0, checksum = 0;
    Decoded d;
    char lenbuf[2];
    while (in.read(lenbuf, 2)) {
        const std::size_t len =
            (static_cast<unsigned char>(lenbuf[0]) << 8) | static_cast<unsigned char>(lenbuf[1]);
        std::vector<char> buf(len);
        if (!in.read(buf.data(), static_cast<std::streamsize>(len))) {
            std::fprintf(stderr, "truncated message at byte %llu\n",
                         static_cast<unsigned long long>(bytes));
            return 1;
        }
        bytes += 2 + len;
        ++messages;
        if (decode(buf, d))
            checksum += d.timestamp + d.order_ref + d.new_order_ref + d.shares + d.price + d.match;
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("messages:   %llu\n", static_cast<unsigned long long>(messages));
    std::printf("bytes:      %llu\n", static_cast<unsigned long long>(bytes));
    std::printf("checksum:   %016llx\n", static_cast<unsigned long long>(checksum));
    std::printf("time %.3f s   %.1f M msgs/s   %.2f GB/s   %.2f ns/msg\n", s, messages / s / 1e6,
                bytes / s / 1e9, s * 1e9 / messages);
}
