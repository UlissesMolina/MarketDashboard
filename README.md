# market — low-latency Nasdaq ITCH 5.0 pipeline

A C++20 market-data pipeline built on Nasdaq's free TotalView-ITCH sample files
(<https://emi.nasdaq.com/ITCH/>): parse the binary feed, keep an order book for every
symbol, deliver it over UDP multicast and measure tick-to-book latency, then compute
microstructure signals on the books and test whether they predict the next move.

| Stage | What | Status |
|---|---|---|
| 1 | Zero-copy ITCH parser + throughput benchmark | done |
| 2 | Order books for every symbol, with invariant checks | done |
| 3 | UDP multicast replayer + real-time consumer, tick-to-book latency | done |
| 4 | Real-time microstructure signals, with an offline predictive test | done |

## Layout

```
include/itch/endian.hpp           BigEndian<N>: alignment-1 big-endian fields, decoded on access
include/itch/messages.hpp         all ITCH 5.0 message structs, sizes pinned by static_assert
include/itch/parser.hpp           parse(begin, end, handler): in-place walk + switch dispatch
include/itch/mapped_file.hpp      RAII read-only mmap
include/book/order_map.hpp        open-addressing order-ref -> live order map
include/book/levels.hpp           L2 price levels: VectorLevels and MapLevels policies
include/book/book_builder.hpp     per-symbol books + feed invariant checks, as a parse handler
include/net/mold.hpp              MoldUDP64 header, packetizer (file slices -> packets)
include/net/seq_tracker.hpp       session lock, gap / duplicate / overlap detection
include/net/feed_handler.hpp      socket-free packet -> books step, shared by tests and consumer
include/net/udp.hpp               multicast sockets, recvmmsg batch with kernel timestamps
include/net/os.hpp                clocks and CPU pinning
include/stats/histogram.hpp       fixed-size log-linear latency histogram
include/signals/signals.hpp       imbalance, microprice, OFI, signed trade flow, decaying sums
include/signals/signal_engine.hpp per-symbol signal state, a BookBuilder listener
include/signals/study.hpp         forward labels (next move, horizon return) and evaluation

tools/itch_stats.cpp              zero-copy benchmark: per-type counts, checksum, msgs/s
tools/itch_naive.cpp              ifstream + per-message allocation baseline, independent decoder
tools/itch_profile.cpp            measures ref range, peak live orders, locates, price ranges
tools/book_replay.cpp             full-day book rebuild: invariants, audits, closing crosses
tools/feed_replayer.cpp           file -> paced MoldUDP64 multicast (zero-copy sendmsg)
tools/feed_consumer.cpp           multicast -> books, tick-to-book latency report
tools/feed_bench.sh               runs consumer + replayer together
tools/signal_study.cpp            offline test: does each signal predict the next move?

tests/test_parser.cpp             hand-built wire bytes
tests/test_book.cpp               order map, level policies, book invariants
tests/test_net.cpp                framing, sequencing, drop/dup/overlap through the books
tests/test_udp.cpp                loopback multicast smoke test (real sockets)
tests/test_signals.cpp            formulas, engine, labeler, evaluation
```

Unit tests run under ASan/UBSan; everything builds with `-Wall -Wextra -Wpedantic -Werror`.

## Build and run (Linux / WSL2)

```bash
./build.sh test                                   # build all, run unit tests
./build/itch_stats ~/itch/01302019.NASDAQ_ITCH50  # zero-copy parse benchmark
./build/itch_naive ~/itch/01302019.NASDAQ_ITCH50  # baseline; checksum must match
./build/book_replay ~/itch/01302019.NASDAQ_ITCH50 --levels vector --repeat 5
bash tools/feed_bench.sh ~/itch/01302019.NASDAQ_ITCH50 --rate 5e6   # full day over multicast
bash tools/feed_bench.sh ~/itch/01302019.NASDAQ_ITCH50 \
    --speed 1 --from 09:29:30 --to 09:35:00 --batch-ns 10000         # real-time market open
./build/signal_study ~/itch/01302019.NASDAQ_ITCH50                 # predictive test, 10 liquid symbols
./build/book_replay ~/itch/01302019.NASDAQ_ITCH50 --signals        # cost of the signal engine
```

- **Data.** Download a `*.NASDAQ_ITCH50.gz` file from the link above and `gunzip -k`
  it. Keep it on the Linux filesystem (`~/...`), not `/mnt/c`; WSL's Windows-drive
  mount is far too slow for mmap benchmarks. Warm the page cache first
  (`cat FILE > /dev/null`).
- **`book_replay`** prints a parse-only pass time; if that is far above ~3 s the run
  is I/O-bound and the book timings are meaningless. It exits nonzero on any hard
  invariant violation.
- **Multicast runs.** Raise the socket buffer cap first (it resets when WSL restarts):
  `wsl -u root sysctl -w net.core.rmem_max=268435456`. The consumer prints the buffer
  it actually got; "capped by net.core.rmem_max" means drops will come early.
  `feed_bench.sh` keeps the feed on 127.0.0.1 (override with `IFACE=`), passes the
  replayer's `--from` to the consumer, and exits nonzero if either side fails.
- **`feed_consumer --signals`** (pass it via `CONSUMER_ARGS="--signals"` to
  `feed_bench.sh`) computes signals live for every symbol and prints the final
  values for the `--show` symbols (default AAPL,MSFT,QQQ).
- **`signal_study`** takes `--horizon-ms` (forward-return horizon, default 1000) and
  `--tau-ms` (decay constant, default 1000) and prints per-symbol and pooled results.

## Design notes

### Stage 1: parser

- **Zero copy.** Each message struct is made only of `char` and `BigEndian<N>`
  members (all alignment 1), so `sizeof` equals the wire length and a pointer into
  the mmapped file can be viewed as the struct directly. Fields are byte-swapped
  only when read, so a handler pays only for the fields it touches.
- **Compile-time handler filtering.** `deliver<Msg>` uses a `requires` check: if
  the handler has no `on(const Msg&)`, that branch compiles to just the length check.
- **Correctness.** Every known message's length is checked against the spec size,
  and parsing stops with `bad_length` or `truncated` rather than reading past the
  buffer. The zero-copy parser and the naive one share no decoding code but must
  produce the same checksum over timestamps, refs, shares, prices and match numbers.

### Stage 2: order books

- **Order map.** Open addressing with linear probing, Fibonacci hashing of the
  order ref, and backward-shift deletion instead of tombstones. ~95% of traffic is
  add/delete/replace, so the table sees hundreds of millions of insert/erase cycles,
  and tombstones would keep lengthening the probe chains. The default 2^22 slots hold
  the day's 1.74M peak live orders below the 0.7 load-factor limit, so it never resizes.
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
- **Top-of-book hash.** A running hash of every symbol's top of book after every
  update lets two level policies, or two delivery paths, be compared message by message.
- **External sanity check.** Each symbol's official closing-cross price is
  compared with its own displayed book at the 16:00 bell. This is not an invariant,
  because on-close orders never display, but a broken book would fail it badly.

### Stage 3: multicast feed

- **MoldUDP64 = file slices.** Mold message blocks use the same 2-byte-length
  framing as the sample file, so a packet is a 20-byte header iovec plus a pointer
  into the mmap, sent with one `sendmsg`. The consumer passes the payload to the
  unchanged stage 1 parser and stage 2 `BookBuilder`.
- **Tick-to-book is measured from the kernel receive timestamp** (`SO_TIMESTAMPNS`),
  the closest software stand-in for a NIC hardware timestamp, to the moment the
  packet's last message has been applied. It is split into `queue` (kernel receive
  to `recvmmsg` return), `book` (to books updated, including packets ahead of it in
  the same batch) and `svc` (this packet's own parse + book time).
- **Losses are counted, never repaired.** Mold sequence gaps, including the
  end-of-session tail check, fail the run, as do malformed or truncated packets and
  hard book invariants. The kernel drop counter (`SO_RXQ_OVFL`) is reported too, but
  it only appears once nonzero and lags. There is no gap recovery.
- **Socket-free core.** `FeedHandler` holds all per-packet logic. Unit tests drive it
  with dropped, duplicated and overlapping packets and compare the resulting
  top-of-book hash with a direct parse.
- **Pacing.** A token bucket enforces `--rate`, and `--max-burst-us` bounds how much
  the sender may catch up after a stall, so a VM hiccup does not become an artificial
  burst. With `--speed`, packets follow the ITCH timestamps, bundling messages within
  `--batch-ns`; a packet more than 1 ms late re-bases the schedule so later bursts keep
  their real spacing. The replayer pre-scans the send range first, which bounds
  `--limit`/`--to` exactly and faults the pages in before timing starts.
- **Clock steps.** Latency samples where the realtime clock visibly stepped
  (negative, or over 10 s) are excluded and counted.

### Stage 4: signals

- **Listener hook.** `BookBuilder<Levels, Listener>` calls the listener after every
  book update and before each execution changes the book. With the default
  `NullListener` the hooks compile away; the top-of-book hash is the same with or
  without a listener.
- **Signals.** Top-of-book imbalance; microprice, which leans the mid toward the thin
  side (microprice − mid = spread/2 · imbalance, so their signs agree);
  Cont–Kukanov–Stoikov order-flow imbalance over each top-of-book change; and signed
  trade flow. OFI and trade flow are exponentially decayed sums in feed time
  (τ = 1 s by default).
- **Exact trade direction.** ITCH executions name the resting order, so the aggressor
  side is known without a Lee–Ready guess. Hidden-order (`P`) trades are excluded,
  since their side field has been fixed to `B` since 2014.
- **Honest evaluation.** Each sample, taken at a top-of-book change during regular
  hours, is labeled with the direction of the next mid change and the mid return over
  a fixed horizon. Labels only use later data. Deciles and correlation are computed
  per symbol before pooling, so share-denominated signals compare fairly across symbols.

## Results

Nasdaq ITCH 5.0, 2019-01-30 (11.25 GB uncompressed, 368,366,634 messages).
AMD Ryzen 7 7800X3D under WSL2 (Ubuntu 24.04, kernel 6.6, g++ 13.3,
`-O3 -march=native`), file already in page cache.

### Parser

Single thread:

| Parser | Time | Throughput | ns/msg |
|---|---|---|---|
| naive (`ifstream`, alloc per message, byte-loop decode) | 15.25 s | 24.2 M msgs/s, 0.74 GB/s | 41.4 |
| zero-copy (mmap + in-place structs), best of 5 | 2.18 s | 168.8 M msgs/s, 5.15 GB/s | 5.9 |

Both produce checksum `61ba59b4ec2b1e01`. The full file parses with no framing
errors, no unknown message types, and timestamps that never decrease.

Message mix: Add 44.2%, Delete 43.0%, Replace 7.4%, Cancel 1.3%, Executed 2.2%,
NOII 1.0%. About 95% of traffic is add/delete/replace, so the book's hot path is
the order-ref lookup and price-level update, not trade handling.

### Order books

Full day, all symbols, single thread, best of 5:

| Level policy | Best | Median | Throughput | ns/msg |
|---|---|---|---|---|
| `VectorLevels` (sorted vector, best at back) | 27.8 s | 28.9 s | 13.3 M msgs/s | 75.4 |
| `MapLevels` (`std::map`) | 43.9 s | 46.0 s | 8.4 M msgs/s | 119.3 |

- Both policies produce top-of-book hash `fea88aae34305502`.
- Every hard invariant is 0: unknown refs, overfills, locate mismatches, duplicate
  adds, level mismatches, and crossed or locked books during open trading. 377
  crossed books occurred pre/post-market or during halts, where they are allowed.
- The three audits (669,566, 1,642,242 and 1,623,555 live orders) find 0 level
  mismatches. Peak live orders: 1,742,866; live at end of day: 0.
- Closing crosses: 2,596 symbols had cross volume and a two-sided 16:00 book.
  91.5% crossed inside that book; |cross − mid| p50 8.2 bps, p90 95 bps. AAPL
  was 165.24 × 165.27 at the bell and crossed at 165.25.
- These timings predate a fix to `MappedFile::prefault()`, which used to be optimized
  away at `-O3`, so they may include page faults. They have not been re-measured.

### Multicast feed

Replayer and consumer on separate pinned cores, multicast over 127.0.0.1, consumer
receive buffer 256 MB. Latency is per packet, in microseconds, from the kernel
receive timestamp.

**Full day.** At `--rate 5e6`: 368,366,634 messages in 8,125,973 packets
(45.3 msgs/packet) in 74.7 s. 0 gaps, 0 kernel drops, 0 malformed, every hard
invariant 0, and top-of-book hash **`fea88aae34305502`**, identical to
`book_replay` on the file: every message arrived exactly once and in order.
Latency: total p50 6.2, p99 56, p99.9 56,885; svc p50 3.8, p99 9.3. The p99.9 is
queueing in the day's heaviest stretches, where 5 M msgs/s exceeds what the consumer
sustains.

**Rate sweep**, first 50 M messages of the day (pre-market and the open):

| Replay rate | Achieved | total p50 | p99 | p99.9 | queue p99 | svc p50 | drops / gaps |
|---|---|---|---|---|---|---|---|
| 1 M/s | 1.00 M/s | 6.6 | 22.9 | 58 | 12.9 | 4.1 | 0 / 0 |
| 2 M/s | 2.00 M/s | 6.6 | 32.3 | 3,228 | 22.7 | 4.1 | 0 / 0 |
| 5 M/s | 4.93 M/s | 6.0 | 28,836 | 60,817 | 28,574 | 3.6 | 0 / 0 |
| 10 M/s | 9.34 M/s (sender-bound) | 168,821 | 406,847 | 425,722 | 406,847 | 3.8 | 0 / 0 |
| unlimited | 9.37 M/s (sender-bound) | 179,306 | 421,528 | 422,817 | 421,528 | 3.8 | 0 / 0 |

The consumer's own cost is flat at every rate (svc p50 3.6–4.1 µs per ~45-message
packet, ~85 ns/msg); what changes is queueing. It keeps up at 1–2 M msgs/s, queues
for tens of ms in bursts at 5 M/s, and falls steadily behind at the sender's
~9.4 M/s ceiling. The 256 MB buffer absorbs the 0.4 s backlog with zero loss.

**Real-time market open**, 09:29:30–09:35:00 at `--speed 1`:

| `--batch-ns` | packets | total p50 | p90 | p99 | p99.9 | p99.99 | queue p99 | svc p50 / p99 |
|---|---|---|---|---|---|---|---|---|
| 10000 | 4,753,224 | 2.73 | 3.81 | 14.97 | 48.4 | 183 | 12.8 | 0.29 / 1.95 |
| 0 (identical timestamps only) | 7,417,069 | 2.65 | 3.50 | 15.04 | 46.6 | 297 | 13.3 | 0.22 / 1.07 |

At the real open the consumer is far from saturated: parse + book takes 0.2–0.3 µs
per packet, and the ~2.5 µs floor is the kernel/socket path. With `--batch-ns 0` the
sender (one ~3 µs `sendmsg` per distinct timestamp) cannot hold the real schedule
through the opening bursts (send lateness p99 823 µs, 592 re-bases), so that run's
bursts are flattened. With a 10 µs window it stays on schedule (p99 14 µs, 7 re-bases),
so that run is the representative one.

**Caveats.** WSL2 is a VM; this is kernel UDP over loopback with no NIC, and sender
and receiver share the machine. Kernel timestamps use `CLOCK_REALTIME`, which Hyper-V
TimeSync steps about every 30 s. Visible steps are excluded, but single `max` values
(about 5 s in two runs) are clock artifacts, so the tables stop at p99.9/p99.99.
Percentiles over millions of packets are unaffected.

### Signals

**Throughput cost.** `book_replay`, full day, all symbols, best of 5:

| | Best | Median | Throughput | ns/msg |
|---|---|---|---|---|
| books only | 29.29 s | 29.96 s | 12.6 M msgs/s | 79.5 |
| books + signal engine | 35.58 s | 37.59 s | 10.4 M msgs/s | 96.6 |

Both produce top-of-book hash `fea88aae34305502`. The engine adds ~17 ns/msg
(~21%) while handling 174,890,436 top-of-book changes and 8,249,299 executions.

**Predictive power.** `signal_study`: AAPL, MSFT, AMZN, GOOGL, FB, NVDA, INTC, CSCO,
QQQ, TSLA, 09:35–15:55, one sample per top-of-book change, pooled over symbols.
Hit rate is the share of samples whose sign matches the next mid move (samples with a
zero signal are skipped); deciles are the mean 1 s forward mid return.

| Signal | samples | hit rate (next move) | corr (1 s fwd) | bottom-decile bps | top-decile bps |
|---|---|---|---|---|---|
| imbalance | 5,713,468 | 63.7% | +0.178 | −0.40 | +0.40 |
| microprice | 5,713,468 | 63.7% | +0.171 | −0.39 | +0.40 |
| OFI | 5,713,468 | 54.4% | +0.078 | −0.13 | +0.13 |
| trade flow | 5,713,468 | 53.6% | +0.045 | −0.17 | +0.15 |

- Imbalance's hit rate is 56.6% (GOOGL) to 66.9% (QQQ) per symbol, and its deciles
  rise monotonically. Microprice matches it exactly, since their signs are identical.
- OFI's deciles rise through the middle but fall back at the extremes; trade flow's
  rise steadily but are small.
- By horizon (the next-move hit rate does not depend on it; correlation does):

| Horizon | imbalance corr | OFI corr | trade flow corr |
|---|---|---|---|
| 100 ms | +0.238 | +0.090 | +0.062 |
| 1 s | +0.178 | +0.078 | +0.045 |
| 10 s | +0.078 | +0.013 | −0.010 |

  The information is short-lived: OFI and trade flow are essentially gone by 10 s.

**Live path.** Real-time open, 09:29:30–09:35:00 at `--speed 1`, `--batch-ns 10000`,
4,753,225 packets, 0 gaps and 0 kernel drops in both runs:

| | total p50 | p99 | p99.9 | svc p50 | svc p99 |
|---|---|---|---|---|---|
| books only | 2.58 | 14.97 | 42.0 | 0.28 | 1.91 |
| books + signals | 2.62 | 20.73 | 43.0 | 0.31 | 2.37 |

The engine adds ~0.03 µs at the median and ~0.5 µs at p99 of per-packet service
time, well below the ~2.5 µs kernel/socket floor.

**Caveat.** One trading day and ten symbols. This is a measurement, not a trading
strategy: predicting the next mid move is not the same as profiting after the spread.
