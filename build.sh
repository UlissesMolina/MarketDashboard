#!/usr/bin/env bash
# Builds everything into build/. Run inside WSL/Linux:  ./build.sh [test]
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build

CXX=${CXX:-g++}
COMMON=(-std=c++20 -Wall -Wextra -Wpedantic -Werror -Iinclude -Itools)
RELEASE=(-O3 -march=native -DNDEBUG -g)
SANITIZE=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)

$CXX "${COMMON[@]}" "${SANITIZE[@]}" tests/test_parser.cpp -o build/test_parser
$CXX "${COMMON[@]}" "${RELEASE[@]}" tools/itch_stats.cpp -o build/itch_stats
$CXX "${COMMON[@]}" "${SANITIZE[@]}" -Itests tests/test_book.cpp -o build/test_book
$CXX "${COMMON[@]}" "${SANITIZE[@]}" -Itests tests/test_net.cpp -o build/test_net
$CXX "${COMMON[@]}" "${SANITIZE[@]}" -Itests tests/test_udp.cpp -o build/test_udp
$CXX "${COMMON[@]}" "${SANITIZE[@]}" -Itests tests/test_signals.cpp -o build/test_signals
$CXX "${COMMON[@]}" "${RELEASE[@]}" tools/itch_naive.cpp -o build/itch_naive
$CXX "${COMMON[@]}" "${RELEASE[@]}" tools/itch_profile.cpp -o build/itch_profile
$CXX "${COMMON[@]}" "${RELEASE[@]}" tools/book_replay.cpp -o build/book_replay
$CXX "${COMMON[@]}" "${RELEASE[@]}" tools/feed_replayer.cpp -o build/feed_replayer
$CXX "${COMMON[@]}" "${RELEASE[@]}" tools/feed_consumer.cpp -o build/feed_consumer
$CXX "${COMMON[@]}" "${RELEASE[@]}" tools/signal_study.cpp -o build/signal_study

if [[ "${1:-}" == "test" ]]; then
    ./build/test_parser
    ./build/test_book
    ./build/test_net
    ./build/test_udp
    ./build/test_signals
fi
