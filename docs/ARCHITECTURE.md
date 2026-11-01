# Architecture

The design follows the public material of large HFT firms and exchanges (see
the sources in [PROJECT_FLOW.md](PROJECT_FLOW.md)). This file says what each
part does and the rules it follows. The choices that were marked **decide**
were made from measurements during the build; each says what was decided,
why, and which results file holds the numbers.

## The pipeline

```
ITCH file ─► Feed handler ─► Order books ─► Signals ─► Strategy ─► Risk checks ─► Exchange simulator
                                                          ▲                                  │
                                                          └──────── acks and fills ◄─────────┘
```

- **Sequencer:** gives every event (market message, order reaching the
  exchange, response reaching the strategy) one global sequence number, and
  processes events in time order. At equal times an order arrival goes first,
  then a response, then a market message. A run therefore depends only on the
  file and the configuration, and two runs give the same output log
  (`results/strategy.md`, "Determinism").
- **Hot path:** everything on the top line, on one thread, with no allocation
  (`results/allocations.md`), no locks and no system calls. On Linux the thread
  is pinned with `--pin`; macOS can't pin threads.
- **Off the hot path:** with `--threads`, a single-producer single-consumer
  ring buffer carries fixed-size log records to a second thread, which formats
  them and writes the file. Without it, the hot path formats into a 64 MB
  buffer that is written at the end.

## Data types

| Quantity | Representation | Notes |
|---|---|---|
| Price | integer with 4 implied decimals, as in ITCH | $12.3400 is 123400. Widen to 64 bits for arithmetic such as P&L. |
| Shares | 32-bit unsigned | |
| Order reference | 64-bit unsigned | Unique within the day, but **not** in time order (see Order book). |
| Timestamp | 64-bit nanoseconds since midnight | ITCH sends 6 bytes. |
| Stock | stock locate code (16-bit) | Indexes the array of books. Stock Directory messages map it to the symbol. |
| Money | 64-bit price units times shares | Cash, equity and the loss budget. |

All simulated time comes from the feed's timestamps, never the wall clock. The
cycle counter is used only to measure latency. The strategy's formulas use
`double`; the prices it sends are rounded back to whole ticks.

## Feed handler (`include/itch/`)

- **Input:** a byte buffer. Each message is a 2-byte big-endian length
  followed by the message, whose first byte is its type.
- **Reading the file (changed from the plan).** The plan was to `mmap` the
  file. The day is 8.25 GB and this machine has 8 GB of memory, so the file
  can't stay in the page cache and every 16 KB page becomes a page fault in the
  middle of parsing. The programs read it in 256 MB chunks with `read` into one
  reused buffer (`ChunkedReader`, `MessageSource`); the parser never waits on
  the disk, and a message cut by a chunk boundary moves to the front of the
  buffer. Both are measured in `results/parser.md`.
- **Decoding:** a `switch` on the type, no virtual calls. Each field is
  `memcpy`'d out of the buffer and byte-swapped, not read by casting the buffer
  to a packed struct. Every known type's length is checked; unknown types are
  skipped by their length. A truncated or malformed buffer gives an error
  status, never a read past the end (tested on every prefix of a stream, and
  fuzzed under ASan + UBSan).
- **Output:** decoded structs passed straight to a handler by function call.
  Handlers inherit from `NullHandler` and hide the functions they need, so the
  calls are resolved at compile time.
- **Session:** System Event messages mark the stages of the day: start of
  messages (O), start of system hours (S), start of market hours (Q), end of
  market hours (M), end of system hours (E), end of messages (C).

## Order book (`include/book/`)

Two books with the same interface: `ReferenceBook` (stage 3) and `FastBook`
(stage 4). Facts from stage 1 that drove the design (`results/day_summary.md`):
peak 1,924,078 live orders; reference numbers from 42 to 308,631,072, density
0.454, with 102.5 million of 140.3 million new numbers below an earlier one;
the busiest side of one stock reaches 4,227 price levels, the median 36; most
stocks have stub quotes near $0.0001 and $199,999.99.

- **Reference book:** one `std::unordered_map` from reference number to the
  order's place (numbers are unique across stocks, so one map for the day), and
  per stock a `std::map` of levels for each side, each level a `std::list`.
  Its invariants are checked after every message of the day
  (`results/reference_book.md`).
- **Order pool:** 32-byte nodes, sized at start-up to 1.5 × the stage 1 peak,
  linked through a free list. Each node has its neighbours in the level's FIFO
  queue (32-bit indices, not pointers), its level, and an **arrival number**.
- **Arrival number (added).** Queue position in the exchange simulator needs
  "was this order here before ours?". Reference numbers can't answer that,
  because they aren't assigned in time order, so the book numbers orders as
  they arrive.
- **Order lookup — decided: flat array.** A flat array of 4-byte slots indexed
  by reference number (1.5 GB with headroom) against an open-addressing hash
  table (16-byte slots, at most half full, backward-shift deletion). The flat
  array is much faster on this data: reference numbers that arrive together
  are close together, so their slots share cache lines and pages, while the
  hash spreads every lookup over the whole table. A hash with
  slot = ref mod size, which keeps that locality, came within about 15% of the
  flat array with 40% of its memory and no need to know the largest number in
  advance; the flat array was kept, because a trading server has the memory
  and yesterday's profile (with headroom) gives the largest number. Numbers,
  three runs each: `results/book.md`.
- **Price levels — decided: sorted vector.** Per side, a vector of (price,
  level) sorted with the best price at the back, so activity near the best
  touches the end of the vector. The search walks the 16 levels nearest the
  best, then switches to binary search: 3% of new orders land 1,000 or more
  ticks away (stub quotes), and a linear walk to them was the largest single
  cost in the first version. The alternative, a flat array of 1,024 (or 256)
  tick slots around the best price with a bitmap and an overflow vector, was
  built, tested and measured, and was not faster. Each side reserves its peak
  level count from stage 1 × 1.5, so the vectors never grow during the day.
  Numbers: `results/book.md`, change log.
- **Messages:** add (A, F), execute (E, C), cancel part of an order (X), delete
  (D), replace (U: delete the old order and add the new one on the same side,
  at the back of the queue). Trade messages (P, Q) don't change the displayed
  book. Both books agree after every message of the day
  (`results/book_diff.md`).
- **Output to signals:** after each book message the touched stock's top of
  book is read (best bid and ask with their sizes); the signals keep the
  previous one and act only when it changed.

## Signals (`include/signals/signals.hpp`)

**Order-flow imbalance** (Cont, Kukanov & Stoikov 2014). On each change of
the top of book n, with best bid price and size P^B, q^B and best ask price
and size P^A, q^A:

```
e_n = 1{P^B_n ≥ P^B_(n−1)} q^B_n − 1{P^B_n ≤ P^B_(n−1)} q^B_(n−1)
    − 1{P^A_n ≤ P^A_(n−1)} q^A_n + 1{P^A_n ≥ P^A_(n−1)} q^A_(n−1)
```

- e_n is 0 when either top of book has an empty side.
- **Rolling windows:** fixed time buckets, so each update is constant time
  with fixed memory: the 1-second OFI is the current 100 ms bucket and the nine
  before it, the 10-second OFI the current 1 s bucket and the nine before it.
  The definition is exact, so Python recomputes the same numbers
  (`results/ofi_check.md`).
- Also per stock: best bid and ask, their sizes, twice the mid (an integer),
  and the last uncrossed two-sided mid, which is what positions are marked at.

## Strategy (`include/strategy/`)

**Avellaneda–Stoikov (2008).** With mid-price s, inventory q (shares), risk
aversion γ, volatility σ, order-arrival decay k and session end T = 16:00:

```
reservation price      r = s − q γ σ² (T − t)
total spread           δ_ask + δ_bid = γ σ² (T − t) + (2/γ) ln(1 + γ/k)
```

- **Inputs from the day** (`results/calibration.md`, look-ahead stated there):
  σ from 1-second mid changes; k and A from how often trades reach each
  distance from the mid within a second, λ(δ) = A e^(−kδ); γ chosen so 100
  shares held move r by one tick at 12:45.
- **Quotes:** r ∓ half the spread, rounded away from r to the tick, then kept
  at least one tick off the other side of the market. No quotes on a crossed
  or locked book, or for a halted stock.
- **Optional OFI skew:** r is shifted by β × (1-second OFI), with β from
  regressing the next second's mid change on this second's OFI.
- **One order per side**, 100 shares; requotes when the top of book or the
  inventory changes, at most one new order or replace per side per 100 ms.
  A side that would take the position past ±500 shares isn't quoted.
- **Quoting window:** only between the Q and M events and inside 9:31–15:59,
  which keeps our orders out of the opening and closing crosses.
- The strategy proposes intents and changes its state only for the ones the
  risk layer sends.

## Risk checks (`include/risk/`)

- **Static limits, on every new order and replace:** order size, position per
  stock counting every open order on the same side, a price collar around the
  mid, open orders across stocks, messages per second (new, replace and cancel
  together, counted on feed time). Cancels always pass, since they only reduce
  risk, but count toward the rate.
- **Loss-limit kill switch:**
  - equity is cash plus each position marked at its mid; z = L − (peak − equity);
  - an order that could make the position larger in size is allowed only if
    its worst case keeps z ≥ 0. Worst case, chosen here: it fills at its own
    price, and the mid then moves m ticks against the position the side would
    have if every open order on that side filled:
    `Q × (price beyond the mid, if any) + m × tick × |position after|`;
  - an order that can't make the position larger in size is always allowed:
    reducing risk must stay possible when the budget is low;
  - when z reaches 0, every open order is cancelled once and quoting stops for
    the rest of the day, like the absorbing FAIL state in the safe RL research.
  - Where the analogy stops: the position left after the kill switch still
    moves with the market, cancels take the latency to arrive, and prices can
    jump more than m ticks, so z can end below 0.
- **One path out:** the exchange gateway's `send` is private and only
  `RiskGate` is a friend, so no other code can compile a call to it
  (a `static_assert` in `tests/test_risk.cpp` checks this).

## Exchange simulator and sequencer (`include/sim/`)

- **Latency:** each order reaches the exchange a fixed, configurable delay
  after the event that triggered it, and each response reaches the strategy
  the same delay after the exchange produced it.
- **Post-only orders:** an order that would cross the book when it arrives is
  rejected (the price moved while it was on its way). A replace cancels the
  old order and places the new one at the back of the queue.
- **Queue position:** our order joins the back of its level: every share
  resting there is ahead of it. Shares ahead leave through executions, cancels
  and deletes of orders with a lower arrival number.
- **Fills:**
  - an execution of an order behind us at our price: we'd have been first, so
    we fill as many shares as it traded;
  - an execution on our side at a worse price, or a hidden trade beyond our
    price: the market traded through us, and the aggressor would have hit us
    first, for as many shares as it traded there (a sweep is several
    executions, which add up);
  - an order arriving on the other side at or through our price: it would have
    traded with us, up to its size.
  - Executions that are part of a cross (C messages marked non-printable) only
    move our queue position: they trade at a single cross price, not in the
    continuous market.
- **Not modelled:** our own market impact (the replayed market never sees our
  orders), hidden liquidity ahead of us, other traders reacting to us, fees
  and rebates, and the details of the opening and closing auctions.
- **Placement:** in replay mode the simulator runs on the hot-path thread,
  standing in for an exchange that would really be across a network.

## Threads (`include/infra/`)

- **Thread 1, the hot path:** sequencer, feed handler, book, signals,
  strategy, risk checks and simulator.
- **Thread 2 (with `--threads`):** takes 64-byte log records from the SPSC
  queue, formats them and writes the file. The output is byte-for-byte the
  file the single-threaded run writes.
- **SPSC queue:** the producer publishes a slot with a release store of the
  tail; the consumer's acquire load of the tail makes the slot visible before
  it reads it, and the same pair on the head frees slots. The two indices sit
  on separate 128-byte lines, and each side keeps a private copy of the
  other's index, reloading it only when the queue looks full or empty.
- **Why not one thread per stage:** every queue hop costs time; stage 9
  measures how much (`results/queues.md`).

## Network, the stretch goal (`include/net/`, `src/apps/mold_*.cpp`)

- **MoldUDP64:** each packet carries a session name, the sequence number of
  its first message and a message count, then length-prefixed ITCH messages; a
  count of 0 is a heartbeat and 0xFFFF ends the session. Packets stay under
  1,400 bytes.
- **Replayer:** sends the start of the day, paced (a real feed is paced by the
  market; unpaced, the kernel drops packets when the receiver's socket buffer
  fills), drops a share of packets on purpose, and runs a re-request server
  that resends any range of messages to whoever asks.
- **Receiver:** delivers messages in sequence-number order. A packet past the
  next expected number opens a gap: it is held, and the missing range (at most
  1,000 messages) is requested, again after 50 ms if nothing arrives. Held
  packets are delivered once the gap closes; duplicates are dropped. At the
  end, the count and hash of everything delivered are compared with the file.

## Measurement points

| Point | Taken |
|---|---|
| t0 | before a message is decoded |
| t1 | after the book update |
| t2 | after the signal update |
| t3 | after the strategy decision |
| t4 | after the risk checks |
| t5 | when the order is handed to the exchange |

Stage latency is the difference between neighbouring points. End-to-end
latency is t5 − t0, for messages that lead to an order. The histograms are
preallocated (64 buckets per power of two, within 1.6%), recording starts at
9:30 (the pre-market replay is the warm-up), and every report names the
machine, compiler and flags. The counter is `rdtsc` on x86 and `cntvct_el0`
on ARM, which ticks only every 41.7 ns on Apple Silicon.

## How correctness is checked

- The fast books are compared with the reference book after every message of
  the day, and in full every 20 million messages.
- Book invariants are checked after every message of the day.
- Unit tests for every message type (typed tests run on every book), every
  risk rule at and past its limit, the kill switch, the strategy formulas
  against hand calculations, and hand-built simulator scenarios.
- A randomized test drives every fast book and the reference book with the
  same random messages and compares them after each one.
- System tests on a synthetic day: two runs give the same log, the threaded log
  equals the single-threaded one, and the run allocates nothing.
- Sanitizers (ASan + UBSan, TSan) run every test; the parser is fuzzed.
- The C++ OFI is checked against the order-flow study's Python code.

## Code map

| Stage | Files |
|---|---|
| 0 Setup | `CMakeLists.txt`, `CMakePresets.json`, `.github/workflows/ci.yml` |
| 1 The data | `tools/download_itch.py`, `src/apps/day_summary.cpp` |
| 2 Feed handler | `include/itch/` (`parser.hpp`, `messages.hpp`, `source.hpp`), `include/common/chunked_reader.hpp`, `bench/parse_bench.cpp`, `tests/fuzz/` |
| 3 Reference book | `include/book/reference_book.hpp`, `src/apps/book_check.cpp` |
| 4 Fast book | `include/book/fast_book.hpp`, `order_index.hpp`, `sorted_levels.hpp`, `tick_levels.hpp`, `src/apps/book_diff.cpp`, `bench/book_bench.cpp`, `bench/legacy/` |
| 5 Signals | `include/signals/signals.hpp`, `src/apps/signals_export.cpp`, `analysis/ofi_check.py` |
| 6 Strategy | `include/strategy/`, `src/apps/calibrate.cpp` |
| 7 Risk | `include/risk/`, `bench/risk_bench.cpp` |
| 8 Simulator | `include/sim/exchange.hpp`, `include/sim/system.hpp` (sequencer) |
| 9 Threads | `include/infra/spsc_queue.hpp`, `include/infra/log.hpp`, `bench/queue_bench.cpp`, `src/apps/alloc_counter.cpp` |
| 10 Measurement | `include/common/clock.hpp`, `include/common/histogram.hpp`, `src/apps/trade.cpp --measure` |
| 11 Results | `src/apps/trade.cpp`, `tools/reproduce.sh` |
| Stretch: network | `include/net/moldudp64.hpp`, `src/apps/mold_replay.cpp`, `src/apps/mold_receive.cpp`, `tools/network_demo.sh` |
