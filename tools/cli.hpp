#pragma once

// Small command-line helpers shared by the feed tools.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace cli {

// "HH:MM:SS" -> nanoseconds since midnight (ITCH timestamp units). Exits on bad input.
inline std::uint64_t parse_hms(const char* s) {
    int h = 0, m = 0, sec = 0;
    if (std::sscanf(s, "%d:%d:%d", &h, &m, &sec) != 3 || h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) {
        std::fprintf(stderr, "bad time '%s' (want HH:MM:SS)\n", s);
        std::exit(2);
    }
    return ((static_cast<std::uint64_t>(h) * 60 + static_cast<std::uint64_t>(m)) * 60 + static_cast<std::uint64_t>(sec)) *
           1'000'000'000ULL;
}

}  // namespace cli
