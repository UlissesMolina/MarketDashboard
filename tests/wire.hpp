#pragma once

// Builds framed ITCH messages byte by byte for tests: 2-byte BE length, then body.

#include <cstdint>
#include <string_view>
#include <vector>

namespace test {

struct Wire {
    std::vector<unsigned char> bytes;
    std::size_t body_start = 0;

    Wire& begin(char type) {
        body_start = bytes.size();
        bytes.push_back(0);
        bytes.push_back(0);
        bytes.push_back(static_cast<unsigned char>(type));
        return *this;
    }
    Wire& be(std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) bytes.push_back(static_cast<unsigned char>(v >> (8 * i)));
        return *this;
    }
    Wire& ch(char c) {
        bytes.push_back(static_cast<unsigned char>(c));
        return *this;
    }
    Wire& str(std::string_view s, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) ch(i < s.size() ? s[i] : ' ');
        return *this;
    }
    Wire& header(char type, std::uint16_t locate, std::uint16_t tracking, std::uint64_t ts) {
        return begin(type).be(locate, 2).be(tracking, 2).be(ts, 6);
    }
    Wire& end() {
        const std::size_t len = bytes.size() - body_start - 2;
        bytes[body_start] = static_cast<unsigned char>(len >> 8);
        bytes[body_start + 1] = static_cast<unsigned char>(len);
        return *this;
    }
    const unsigned char* data() const { return bytes.data(); }
    const unsigned char* data_end() const { return bytes.data() + bytes.size(); }
};

}  // namespace test
