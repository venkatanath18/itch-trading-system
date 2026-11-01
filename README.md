# C++ Trading System, from Market Data to Order

[![CI](https://github.com/venkatanath18/itch-trading-system/actions/workflows/ci.yml/badge.svg)](https://github.com/venkatanath18/itch-trading-system/actions/workflows/ci.yml)

A trading system in C++20 that goes from an exchange's raw market data to an
order, run on a full day of real Nasdaq data: 268,744,780 TotalView-ITCH 5.0
messages from 30 December 2019. It parses the binary feed, keeps a per-order
book for every stock, computes an order-flow signal, quotes a market-making
strategy, checks every order against risk limits and a loss-limit kill switch,
and fills the orders in a simulated exchange with exact queue positions.
Every fast part is checked against a slow, obviously correct one, and every
stage is timed.

**Research inside it.**
- The order-flow imbalance (OFI) signal from my order-flow study (`../p1-order-flow`),
  checked against that study's own Python code.
- Avellaneda–Stoikov quotes, the model studied in my market-making paper.
- A kill switch built on the "only affordable actions" result from my safe
  reinforcement learning research: an order is allowed only if its worst case
  keeps the remaining loss budget non-negative.

**Builds on** my earlier C++ limit order book and lock-free MPMC queue, which
are measured against the new ones (`bench/legacy/`).

## Results at a glance

All numbers are from one Apple M1 with 8 GB of memory (see each file for the
machine, compiler and flags). The counter on this machine ticks every 41.7 ns,
so the latency percentiles below are coarse; see Limitations.

| | Result | Source |
|---|---|---|
| Feed handler | every message decodes; **8.21 ns per message** (122 million a second), parsing only | [parser.md](results/parser.md) |
| Fast book against the reference book | **0 differences**, compared after each of the 263,241,937 book messages | [book_diff.md](results/book_diff.md) |
| Time per book message, mean | fast **99.7 ns** (median of 3 runs), reference 398.6 ns, my earlier book 500.4 ns | [book.md](results/book.md) |
| OFI against the order-flow study's code | **identical** on all 87,012 AAPL top-of-book changes in the test hour | [ofi_check.md](results/ofi_check.md) |
| OFI explains 10-second mid-price moves | R² **0.81 to 0.88** for AAPL, MSFT, INTC and CSCO (0.43 for AMZN) | [ofi_check.md](results/ofi_check.md) |
| Avellaneda–Stoikov over the day, no kill switch | **−$5,839.74** on 849,413 shares traded | [strategy.md](results/strategy.md) |
| Kill switch (loss limit $500) | fired in **7 of 7** runs, each time between 9:34 and 9:35 | [strategy.md](results/strategy.md) |
| A message that leads to an order, end to end | **333 ns median, 658 ns mean**, from before decoding to the order handed to the exchange | [latency.md](results/latency.md) |
| Memory allocations during the day's run | **0** | [allocations.md](results/allocations.md) |
| Determinism | two runs, and a run with the log on a second thread, write the **same log** (hash `7cb837c2034cdace`) | [strategy.md](results/strategy.md) |
| Tests | 172 tests pass in Debug and Release, 171 under ASan + UBSan and under TSan | [tests.md](results/tests.md) |
| Stretch: the feed over UDP | 10,000,000 messages as MoldUDP64 with 0.1% of packets dropped: all 253 gaps found and filled, the result **identical to the file** | [network.md](results/network.md) |

## What the day showed

**The data shaped the design** ([day_summary.md](results/day_summary.md)).
At the busiest moment, 15:55:58, 1,924,078 orders were live. Order reference
numbers run from 42 to 308,631,072, but they are not handed out in time order:
102.5 million of the 140.3 million new numbers were below an earlier one. So
the simulator can't use them to tell which order came first, and the book
numbers orders as they arrive. Most stocks also have "stub quotes" near
$0.0001 and $199,999.99, so 3% of new orders land 1,000 or more ticks from
the best price ([reference_book.md](results/reference_book.md)).

**Correctness.** The reference book (standard containers) checks its
invariants after every one of the 263,241,937 book messages: none fail. During
market hours the best bid reached the best ask 33 times, all explained: 2 at
the opening cross, 31 while a stock was halted. Four fast books, using the two
order-lookup designs and two price-level designs, then match the reference
book after every message of the day.

**Speed** ([parser.md](results/parser.md), [book.md](results/book.md),
[latency.md](results/latency.md)).
- Decoding costs 8.21 ns a message. Reading the 8.25 GB file through `mmap` on
  an 8 GB machine made it 70.57 ns, because every page faults; the system reads
  it in 256 MB chunks instead. The same messages decode in 4.54 ns sorted by
  type and 8.64 ns shuffled: up to half the cost is the branch predictor
  guessing the next message type, and in the real feed it guesses almost as
  badly as for a random order (8.41 ns).
- The book is the slowest stage. The change log in `book.md` records what each
  design choice bought, each measured three times over the whole day:
  - finding an order through a flat array indexed by its reference number
    instead of a hash table took the mean from 159.3 to 99.7 ns, because
    numbers that arrive together then sit together in memory. A hash table
    that keeps that locality (slot = number mod size) reached 114.7 ns with
    40% of the memory; the flat array was kept;
  - searching the price levels by binary search past the 16 best saved 15 ns
    with the hash table (174.3 to 159.3 ns), but made no measurable difference
    with the flat array (98.0 against 99.7 ns);
  - a flat array of price ticks around the best price (196.2 ns) was slower
    than the sorted vector.
- My earlier book, which allocated a `shared_ptr` for every order and keyed
  levels by `double`, averaged 500.4 ns a message, 5 times the fast book.
- Past the book, the signal, strategy, risk and hand-off stages each measure
  about 40 ns on average, of which reading the timer itself is 10 to 20 ns on
  this machine.

**The research results.**
- *OFI in equities* ([ofi_check.md](results/ofi_check.md)): normalised OFI
  explains 81–88% of 10-second mid-price changes for four large stocks, above
  the ~65% Cont, Kukanov & Stoikov report and the 53–59% my order-flow study
  found for Bitcoin and Ether. It explains far less for AMZN (43%), where only
  52 shares sit at the best prices. As in the crypto study, it barely
  forecasts the next interval: out of sample, R² is below 1% for every stock
  and negative in 5 of the 10 cases.
- *Avellaneda–Stoikov loses money on this day* ([strategy.md](results/strategy.md)).
  With parameters estimated from the day itself, it lost $5,839.74 over
  849,413 shares, most of it in AAPL. The model assumes the price moves
  independently of when our orders fill. In the replay, our orders fill mostly
  when an aggressive order is pushing the price through them, so each fill is
  followed by a move against us (adverse selection). In AAPL, whose spread
  averages 2.9 ticks, the model's quotes often improve the market, so we trade
  with every incoming aggressor first.
- *Latency costs money*: −$5,839.74 at zero latency, −$6,480.32 at 100 µs and
  −$7,589.12 at 1 ms each way. Exchange rejections (an order arriving after
  the price had moved through it, or a replace arriving after its order had
  filled) grew from 1,715 to 6,429.
- *The OFI skew helps a little*: −$5,583.49 instead of −$5,839.74 without the
  kill switch.
- *The kill switch* stopped every run that had it within about four minutes
  of the start of quoting, cutting the day's loss to between −$969.85 and
  +$75.90. But the largest drawdown reached $2,728.04 against a $500 limit:
  after the switch cancels the orders, the position it leaves still moves with
  the market. This is where the analogy with the safe-RL result stops: there,
  the failure state is absorbing and costs nothing more; here, unless the
  switch also flattens the position, the loss can keep growing.

**The feed over the network** ([network.md](results/network.md)). The
stretch goal sends the first 10 million messages as MoldUDP64 packets, the
framing Nasdaq uses for ITCH over multicast, through UDP on localhost, and
drops 0.1% of the packets on purpose. The receiver finds every gap from the
sequence numbers, holds the packets that arrived after it, asks a re-request
server for the missing messages and applies everything in order to the book:
253 gaps, 253 requests, the same bytes as the file, 4.0 million messages a
second. Without pacing, the sender overflowed the receiver's socket buffer
and the kernel dropped thousands of packets more; the first receiver then
asked for up to 60,000 messages at a time and set off a storm of re-sends,
which is why requests are now capped at 1,000 messages.

## Design

```
ITCH file ─► Feed handler ─► Order books ─► Signals ─► Strategy ─► Risk checks ─► Exchange simulator
                                                          ▲                                  │
                                                          └──────── acks and fills ◄─────────┘
```

One thread runs the whole path: a sequencer numbers every event (market
message, order reaching the exchange, response reaching the strategy) and
handles them in time order, so a run depends only on the input. Optionally a
single-producer single-consumer queue carries the log to a second thread.
Prices and quantities are integers; everything is preallocated from the
measured peaks. The risk layer is the only code that can reach the exchange,
enforced by the compiler. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) has the
details and every design decision; [docs/PROJECT_FLOW.md](docs/PROJECT_FLOW.md)
has the stages, their checks and their status.

## Limitations

- **One day of sample data**, a quiet holiday-week day. The strategy's inputs
  (σ, k, the OFI coefficient) are estimated from the same day it trades:
  look-ahead.
- **A replay, not live trading.** The replayed market never sees our orders:
  no market impact, no other traders reacting to us, no hidden liquidity ahead
  of us. No fees or rebates.
- **Latency is measured from memory**, not from the wire: no network card,
  kernel or packet handling (the stretch goal adds a localhost UDP receive
  path, with the kernel's network stack in it).
- **Measured on an Apple M1**, whose counter ticks every 41.7 ns, which can't
  pin threads to cores, and which has less memory than one day of data. The
  final latency numbers should come from a Linux x86 machine with pinned,
  isolated cores, using the same command.

## Reproduce

Needs CMake 3.25 or later, Ninja, a C++20 compiler (Apple clang 17 and GCC 15
tested), Python 3 for the download, and for stage 5 the order-flow study at
`../p1-order-flow` with its environment.

```bash
python3 tools/download_itch.py 12302019   # 3.5 GB download, 8.25 GB decompressed
tools/reproduce.sh 12302019               # every file in results/: builds, then about 50 minutes on an M1
tools/network_demo.sh 12302019            # the stretch goal on its own (reproduce.sh runs it too)
```

## Layout

```
p6-trading-system/
├── docs/        project flow and architecture
├── include/     the system: itch/ book/ signals/ strategy/ risk/ sim/ infra/ net/ common/
├── src/         the programs for each stage (src/apps), and the few .cpp files
├── tests/       unit, randomized, scenario, system and fuzz tests
├── bench/       benchmarks, and the earlier order book and queues (bench/legacy)
├── tools/       download, reproduce, network demo
├── analysis/    the Python cross-check of OFI against the order-flow study
├── data/        market data (not in git; see data/README.md)
└── results/     every number quoted anywhere comes from a file here
```

## Rules

- No memory allocation, no locks and no system calls on the hot path.
- Prices and quantities are integers.
- Every fast component is checked against a slow reference.
- Every number quoted comes from a file in `results/`, with the machine it ran on.
