# market — low-latency Nasdaq ITCH 5.0 pipeline

A C++20 market-data pipeline built on Nasdaq's free TotalView-ITCH sample files
(<https://emi.nasdaq.com/ITCH/>).

| Stage | What | Status |
|---|---|---|
| 1 | Zero-copy ITCH parser + throughput benchmark | done |
| 2 | Order books for every symbol, with invariant checks | next |
| 3 | UDP multicast replayer + real-time consumer, tick-to-book latency | |
| 4 | Real-time microstructure signals | optional |

## Layout

```
include/itch/endian.hpp       BigEndian<N>: alignment-1 big-endian fields, decoded on access
include/itch/messages.hpp     all ITCH 5.0 message structs, sizes pinned by static_assert
include/itch/parser.hpp       parse(begin, end, handler): in-place walk + switch dispatch
include/itch/mapped_file.hpp  RAII read-only mmap
tools/itch_stats.cpp          zero-copy benchmark: per-type counts, checksum, msgs/s
tools/itch_naive.cpp          ifstream + per-message allocation baseline, independent decoder
tests/test_parser.cpp         hand-built wire bytes, run under ASan/UBSan
```

## Build and run (Linux / WSL2)

```bash
./build.sh test                                   # build all, run unit tests
./build/itch_stats ~/itch/01302019.NASDAQ_ITCH50  # zero-copy, warm page cache
./build/itch_naive ~/itch/01302019.NASDAQ_ITCH50  # baseline; checksum must match
```

Data: download a `*.NASDAQ_ITCH50.gz` file from the link above and `gunzip -k` it.
Keep it on the Linux filesystem (`~/...`), not `/mnt/c`; WSL's Windows-drive
mount is far too slow for mmap benchmarks.

## Design notes (stage 1)

- **Zero copy.** Each message struct is made only of `char` and `BigEndian<N>`
  members (all alignment 1), so `sizeof` equals the wire length and a pointer into
  the mmapped file can be viewed as the struct directly. Fields are byte-swapped
  only when read, so a handler pays only for the fields it touches.
- **Compile-time handler filtering.** `deliver<Msg>` uses a `requires` check: if
  the handler has no `on(const Msg&)`, that branch compiles to just the length check.
- **Correctness.** Every known message's length is checked against the spec size,
  and parsing stops with `bad_length` or `truncated` rather than reading past the
  buffer. On the real file, the zero-copy parser and the naive one share no decoding
  code but must produce the same checksum over timestamps, refs, shares, prices
  and match numbers.

## Results

Nasdaq ITCH 5.0, 2019-01-30 (11.25 GB uncompressed, 368,366,634 messages).
AMD Ryzen 7 7800X3D under WSL2 (Ubuntu 24.04, g++ 13.3, `-O3 -march=native`),
single thread, file already in page cache.

| Parser | Time | Throughput | ns/msg |
|---|---|---|---|
| naive (`ifstream`, alloc per message, byte-loop decode) | 15.25 s | 24.2 M msgs/s, 0.74 GB/s | 41.4 |
| zero-copy (mmap + in-place structs), best of 5 | 2.18 s | 168.8 M msgs/s, 5.15 GB/s | 5.9 |

Both produce checksum `61ba59b4ec2b1e01`. The full file parses with no framing
errors, no unknown message types, and timestamps that never decrease.

Message mix: Add 44.2%, Delete 43.0%, Replace 7.4%, Cancel 1.3%, Executed 2.2%,
NOII 1.0%. About 95% of traffic is add/delete/replace, so stage 2's hot path is
the order-ref lookup and price-level update, not trade handling.

Caveats: WSL2 is a VM. The numbers are warm-cache. A cold run (`--cold` after
dropping caches) needs `sudo` and was not measured yet.
