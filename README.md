# market — low-latency Nasdaq ITCH 5.0 pipeline

A C++20 market-data pipeline built on Nasdaq's free TotalView-ITCH sample files
(<https://emi.nasdaq.com/ITCH/>).

| Stage | What | Status |
|---|---|---|
| 1 | Zero-copy ITCH parser + throughput benchmark | done |
| 2 | Order books for every symbol, with invariant checks | done |
| 3 | UDP multicast replayer + real-time consumer, tick-to-book latency | next |
| 4 | Real-time microstructure signals | optional |

## Layout

```
include/itch/endian.hpp       BigEndian<N>: alignment-1 big-endian fields, decoded on access
include/itch/messages.hpp     all ITCH 5.0 message structs, sizes pinned by static_assert
include/itch/parser.hpp       parse(begin, end, handler): in-place walk + switch dispatch
include/itch/mapped_file.hpp  RAII read-only mmap
include/book/order_map.hpp    open-addressing order-ref -> live order map
include/book/levels.hpp       L2 price levels: VectorLevels and MapLevels policies
include/book/book_builder.hpp per-symbol books + feed invariant checks, as a parse handler
tools/itch_stats.cpp          zero-copy benchmark: per-type counts, checksum, msgs/s
tools/itch_naive.cpp          ifstream + per-message allocation baseline, independent decoder
tools/itch_profile.cpp        measures ref range, peak live orders, locates, price ranges
tools/book_replay.cpp         full-day book rebuild: invariants, audits, closing crosses
tests/test_parser.cpp         hand-built wire bytes, run under ASan/UBSan
tests/test_book.cpp           book and invariant tests, run under ASan/UBSan
```

## Build and run (Linux / WSL2)

```bash
./build.sh test                                   # build all, run unit tests
./build/itch_stats ~/itch/01302019.NASDAQ_ITCH50  # zero-copy, warm page cache
./build/itch_naive ~/itch/01302019.NASDAQ_ITCH50  # baseline; checksum must match
./build/book_replay ~/itch/01302019.NASDAQ_ITCH50 --levels vector --repeat 5
```

Warm the page cache first (`cat FILE > /dev/null`). `book_replay` prints a
parse-only pass time; if that is far above ~3 s the run is I/O-bound and the book
timings are meaningless. `book_replay` exits nonzero on any hard invariant violation.

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

## Design notes (stage 2)

- **Order map.** Open addressing with linear probing, Fibonacci hashing of the
  order ref, and backward-shift deletion instead of tombstones. ~95% of traffic is
  add/delete/replace, so the table sees hundreds of millions of insert/erase
  cycles, and tombstones would make the probe chains longer and longer. The default 2^22 slots hold the
  day's 1.74M peak live orders below the 0.7 load-factor limit, so it never resizes.
- **Price levels.** `VectorLevels` keeps a sorted vector with the best price at
  the back and scans from there. Activity clusters at the touch, so scans and
  shifts are short. `MapLevels` (`std::map`) is the textbook baseline. Both are
  template policies, so the same builder benchmarks either one.
- **Invariants are counted, never ignored.** Unknown refs, overfills, locate
  mismatches, duplicate adds, level underflows, and crossed or locked books while
  the market is open and the symbol is trading each get a counter. Crossed books
  pre-open, post-close or during halts are legal and only reported.
- **Independent audit.** At 09:30:01, 12:00:00 and 15:59:59, `audit()` rebuilds
  every level from the live orders and diffs it with the incrementally maintained
  books. Audit time is excluded from the throughput.
- **Policy cross-check.** A running hash of every symbol's top of book after
  every update lets the two level policies be compared message by message.
- **External sanity check.** Each symbol's official closing-cross price is
  compared with its own displayed book at the 16:00 bell. This is not an invariant,
  because on-close orders never display, but a broken book would fail it badly.

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

### Order books

Same file and machine, full day, all symbols, best of 5, warm cache (the parse-only pass takes ~3 s):

| Level policy | Best | Median | Throughput | ns/msg |
|---|---|---|---|---|
| `VectorLevels` (sorted vector, best at back) | 27.8 s | 28.9 s | 13.3 M msgs/s | 75.4 |
| `MapLevels` (`std::map`) | 43.9 s | 46.0 s | 8.4 M msgs/s | 119.3 |

Both policies produce top-of-book hash `fea88aae34305502`.

- Every hard invariant is 0: unknown refs, overfills, locate mismatches, duplicate
  adds, level mismatches, and crossed or locked books during open trading. 377
  crossed books occurred pre/post-market or during halts, where they are allowed.
- The three audits (669,566, 1,642,242 and 1,623,555 live orders) find 0 level
  mismatches. Peak live orders: 1,742,866; live at end of day: 0.
- Closing crosses: 2,596 symbols had cross volume and a two-sided 16:00 book.
  91.5% crossed inside that book; |cross − mid| p50 8.2 bps, p90 95 bps. AAPL
  165.24 × 165.27 at the bell and crossed at 165.25.

Caveats: WSL2 is a VM. The numbers are warm-cache. A cold run (`--cold` after
dropping caches) needs `sudo` and was not measured yet.
