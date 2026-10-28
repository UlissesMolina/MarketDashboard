// itch_stats: memory-map an uncompressed ITCH 5.0 file, parse it with the
// zero-copy parser, and report per-type counts, a field checksum and throughput.
//
//   itch_stats <file> [--repeat N] [--cold]
//
// By default the file is prefaulted first so the timed passes measure parsing,
// not disk or page-fault cost. --cold skips that and times a single first pass.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "itch/mapped_file.hpp"
#include "itch/parser.hpp"
#include "stats_handler.hpp"

namespace {

const char* error_name(itch::ParseError e) {
    switch (e) {
        case itch::ParseError::none: return "none";
        case itch::ParseError::truncated: return "truncated";
        case itch::ParseError::bad_length: return "bad_length";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <itch-file> [--repeat N] [--cold]\n", argv[0]);
        return 2;
    }
    const std::string path = argv[1];
    int repeat = 3;
    bool cold = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cold")) cold = true;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (cold) repeat = 1;
    if (repeat < 1) repeat = 1;

    itch::MappedFile file(path);
    if (!cold) {
        const auto t0 = std::chrono::steady_clock::now();
        const unsigned sink = file.prefault();
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("prefault: %.2f GB in %.2f s (sink %u)\n", file.size() / 1e9, s, sink);
    }

    std::vector<double> secs;
    tools::StatsHandler stats;
    itch::ParseResult result;
    for (int run = 0; run < repeat; ++run) {
        stats = {};
        const auto t0 = std::chrono::steady_clock::now();
        result = itch::parse(file.data(), file.end(), stats);
        secs.push_back(
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }

    std::printf("\n%-6s %15s %8s\n", "type", "count", "share");
    for (int t = 0; t < 256; ++t) {
        if (!stats.counts[t]) continue;
        std::printf("  %c    %15llu %7.2f%%\n", t, static_cast<unsigned long long>(stats.counts[t]),
                    100.0 * stats.counts[t] / result.messages);
    }

    std::printf("\nmessages:        %llu\n", static_cast<unsigned long long>(result.messages));
    std::printf("bytes parsed:    %zu of %zu\n", result.offset, file.size());
    std::printf("parse error:     %s\n", error_name(result.error));
    std::printf("unknown types:   %llu\n", static_cast<unsigned long long>(stats.unknown));
    std::printf("ts regressions:  %llu\n", static_cast<unsigned long long>(stats.ts_regressions));
    std::printf("checksum:        %016llx\n", static_cast<unsigned long long>(stats.checksum));

    std::sort(secs.begin(), secs.end());
    const double best = secs.front();
    const double median = secs[secs.size() / 2];
    std::printf("\n%s pass(es): %d   best %.3f s   median %.3f s\n", cold ? "cold" : "warm", repeat,
                best, median);
    std::printf("throughput (best): %.1f M msgs/s   %.2f GB/s   %.2f ns/msg\n",
                result.messages / best / 1e6, result.offset / best / 1e9,
                best * 1e9 / result.messages);
    return result.error == itch::ParseError::none ? 0 : 1;
}
