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
$CXX "${COMMON[@]}" "${RELEASE[@]}" tools/itch_naive.cpp -o build/itch_naive

if [[ "${1:-}" == "test" ]]; then
    ./build/test_parser
fi
