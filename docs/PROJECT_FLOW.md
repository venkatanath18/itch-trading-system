# Project Flow: Building the System Stage by Stage

This is the build order. Each stage depends on the ones before it. There are
no dates: you set the pace. A stage is done when every box under **Done when**
is ticked and you can answer its **check questions** out loud, without notes.

> **Status (1 Nov 2026).** Every stage is built and its results are in
> `results/`, on an Apple M1 (8 GB) with the 30 Dec 2019 day; the stretch goal
> too. A ticked box below means the result file exists and says it passed.
> CI passes on GitHub Actions from the first push (1 Nov 2026): Debug,
> Release, ASan + UBSan and TSan, each with clang and GCC on Linux, plus a
> libFuzzer run.
> Still open:
> - **Linux x86 machine:** not used. The latency numbers come from the M1,
>   whose timer ticks every 41.7 ns and which can't pin threads, so take the
>   final ones on Linux (`tools/reproduce.sh` pins with `--pin` there).
> - **Fresh-clone test** of stage 11: clone the repo from GitHub, download the
>   day and run `tools/reproduce.sh`.
> - **The check questions** are yours: the code and `results/` hold the
>   material, but being able to answer them is the point of each stage.

For each stage:
1. **Read** what's listed (mostly [ARCHITECTURE.md](ARCHITECTURE.md) and the
   sources at the end).
2. **Build** it yourself. No library for the core logic: the parser, the
   books, the queues and the matching are yours.
3. **Check** it: unit tests first, then the whole day.
4. **Measure** it, and write the number and the machine into `results/`.
5. **Answer the check questions.**

**Ground rules**
- Correct first, fast second. Every fast component is checked against a slow,
  obviously correct one.
- Measure before optimising, and change one thing at a time.
- No memory allocation, no locks and no system calls on the hot path.
- Prices and quantities are integers.
- Every number you quote comes from a file in `results/`, with the machine it
  ran on.

---

## Stage 0: Setup

**Goal:** a project that builds and tests in every configuration, locally and
in CI, before any trading code exists.

**Build**
- A CMake project in C++20 with four build types: Debug, Release,
  AddressSanitizer + UndefinedBehaviorSanitizer, and ThreadSanitizer.
- A test framework (GoogleTest or Catch2) and a benchmark harness (Google
  Benchmark, or your own loop around a cycle counter).
- Warnings on and treated as errors.
- A git repository, with CI on GitHub Actions (Linux) that builds, runs the
  tests and runs the sanitizer builds.
- Access to a Linux x86 machine for the final latency numbers (a small cloud
  VM is enough). macOS can't pin a thread to a core, and the Apple Silicon
  timer ticks only about every 42 ns.

**Done when**
- [x] An empty test passes in all four configurations, locally and in CI.
  (Locally: `results/tests.md`. CI: all four configurations pass with clang and GCC on Linux.)
- [ ] The project builds and runs on the Linux machine.
  (It builds and passes the tests on Linux in CI; there is no Linux x86 machine for the latency runs yet.)

**Check questions**
- What does each sanitizer catch, and why can't AddressSanitizer and
  ThreadSanitizer run in the same build?
- What do `-O2`, `-O3` and `-march=native` change, and why does that matter
  when you compare benchmark numbers?

---

## Stage 1: The data

**Goal:** one full trading day of Nasdaq TotalView-ITCH 5.0 on disk, verified,
and a first program that walks every message.

**Read:** [data/README.md](../data/README.md); in the ITCH 5.0 specification,
the formats of System Event, Stock Directory, Add Order, Order Executed,
Order Cancel, Order Delete, Order Replace and Trade.

**Build**
- Download one day (start with the smallest file) and check its MD5 against
  Nasdaq's `.md5sum` file.
- A program that reads the file message by message. Each message is preceded
  by a 2-byte big-endian length, and its first byte is the message type.
- Count messages by type over the whole day, and time the run.
- Measure what later stages need:
  - the number of stocks (Stock Directory messages);
  - the largest number of orders alive at once (adds minus deletes and full
    executions; a plain hash map is fine here);
  - the range and density of order reference numbers;
  - the price range per stock.

**Check:** the first message is a System Event; the counts add up to the
total; a second run gives the same counts.

**Done when**
- [x] `results/day_summary.md` holds the file name, its MD5, message counts by
  type, the peak number of live orders, the run time and the machine.

**Check questions**
- What is a stock locate code, and why is it a better book index than the
  8-character symbol?
- How many orders are alive at the busiest moment of the day, and why does
  that number decide your memory layout?
- Why are ITCH integers big-endian, and what does converting them cost?

---

## Stage 2: Feed handler

**Goal:** decode every message the books need, with no copies and no
allocation.

**Read:** ARCHITECTURE.md, "Feed handler"; Carl Cook's CppCon 2017 talk
(measurement first, and knowing what the compiler does).

**Build**
- Decoders for the types the books and the session need:
  - System Event (S), Stock Directory (R), Trading Action (H);
  - Add Order (A, F), Order Executed (E, C), Order Cancel (X),
    Order Delete (D), Order Replace (U);
  - Trade (P) and Cross Trade (Q), for the trade tape.
- Read fields straight from the buffer: big-endian loads, 6-byte timestamps
  (nanoseconds since midnight), prices as integers with 4 implied decimals,
  the stock locate as an index. Skip types you don't need by their length.
- Dispatch on the type with a `switch` or templates, not virtual calls.
- Read the file through `mmap` (or one large buffer), so the parser never
  waits on I/O.

**Check**
- Unit tests: for each message type, hand-built bytes give the expected
  fields.
- Truncated or malformed input: the parser reports an error and never reads
  past the buffer (run under AddressSanitizer).
- A fuzz target. libFuzzer needs a clang that ships it: on Linux, or LLVM
  from Homebrew on macOS.

**Measure:** messages per second and nanoseconds per message over the whole
day, parsing only.

**Done when**
- [x] Every message in the day decodes, and the counts by type match stage 1.
- [x] Tests and fuzzing pass under the sanitizers.
- [x] `results/parser.md` holds the throughput and ns/message, with the
  machine.

**Check questions**
- Why read each field from the buffer instead of casting the buffer to a
  packed struct? (Think about alignment, strict aliasing and endianness.)
- What does a branch mispredict cost, and how does the mix of message types
  affect your `switch`?
- This is the same job as parsing a binary protocol in an embedded driver.
  What carries over, and what is different?

---

## Stage 3: Reference order book (slow and obviously correct)

**Goal:** a book you trust, to check the fast one against.

**Build**
- Per stock: orders in a `std::unordered_map` keyed by reference number, and
  price levels in `std::map` (bids descending, asks ascending), each level a
  FIFO list of orders.
- Apply the book messages:
  - A and F add an order;
  - E and C execute part or all of an order (remove it at zero);
  - X cancels some of its shares;
  - D deletes it;
  - U replaces it: delete the old order, add the new one on the same side, at
    the back of the queue.
- A snapshot: the top N levels on each side, for any stock, after any message.

**Check**
- Invariants after every message:
  - no order with zero or negative shares stays in the book;
  - each level's total equals the sum of its orders;
  - during regular market hours the best bid is below the best ask (count
    any exceptions and explain them).
- Unit tests for every message type. Pay attention to U, which carries no
  side (take it from the original order), and to executions that empty an
  order.

**Done when**
- [x] The whole day replays with every invariant holding, or each exception
  is explained in `results/`.

**Check questions**
- Why does a replaced order lose its place in the queue?
- What is the difference between an E and a C message, and what is the
  "printable" flag on C for?
- Why don't Trade (P) messages change the displayed book?

---

## Stage 4: Fast order book

**Goal:** the same answers as the reference book, as fast as you can make it,
with no allocation after start-up.

**Read:** ARCHITECTURE.md, "Order book"; your stage 1 numbers (peak live
orders, reference-number density, price ranges).

**Build.** Decide each choice from the stage 1 numbers, and write down why.
- An order pool: every order node preallocated, with a free list.
- Order lookup by reference number: an open-addressing hash table sized up
  front, or a flat array if the reference numbers are dense enough.
- Price levels as intrusive doubly linked FIFO lists. For finding a level by
  price, try a flat array of ticks around the current price and a sorted
  vector, and keep the one that measures faster.
- One book per stock, indexed by stock locate.

**Check**
- Differential test: replay the whole day through both books and compare the
  top N levels of every stock after every message. If that's too slow,
  compare at a fixed sample of messages, and say which.
- Every stage 3 unit test passes on the fast book too.

**Measure**
- Nanoseconds per message (median, 99th and 99.9th percentile) and total
  replay time, for the fast book and the reference book.
- **Before and after:** feed the same stream to your earlier C++ order book,
  through a small adapter (add, cancel, modify), and measure it the same way.

**Done when**
- [x] Zero differences from the reference book over the whole day.
- [x] `results/book.md` holds the three books' latencies on the same machine,
  and what each design choice bought.

**Check questions**
- Why integer prices? What goes wrong with `double` as a map key?
- Hash table or flat array for order lookup: what did your data say?
- Where do the cache misses happen in your book, and how do you know?
- Your earlier book allocated a `shared_ptr` for every order. What did that
  cost, measured?

---

## Stage 5: Signals (your research inside the system)

**Goal:** the order-flow imbalance (OFI) signal from the order-flow study,
computed incrementally inside the system.

**Read:** Cont, Kukanov & Stoikov (2014); the OFI section of
`../p1-order-flow/docs/MATH_NOTES.md`; ARCHITECTURE.md, "Signals".

**Build**
- Per stock: best bid and ask, their sizes, the mid-price, and OFI, updated
  on every top-of-book change over rolling windows, in constant time per
  update and with no allocation.

**Check**
- Export one stock's top-of-book changes for one hour from the reference
  book, compute OFI with your Python code from the order-flow study, and
  compare it with the C++ values. They must match.
- Sanity check on equities: regress mid-price changes on OFI for a few
  stocks. Is the relationship as strong as in crypto, and as in Cont et al.'s
  equity results?

**Done when**
- [x] The C++ and Python OFI match on the test hour.
- [x] `results/ofi_check.md` holds the regression for the chosen stocks.

**Check questions**
- Write down the OFI formula and explain each term.
- Why does OFI explain price changes in the same interval well, but forecast
  the next interval poorly?

---

## Stage 6: Strategy

**Goal:** Avellaneda–Stoikov market making on a few stocks, with an optional
OFI skew.

**Read:** Avellaneda & Stoikov (2008); your market-making paper;
ARCHITECTURE.md, "Strategy".

**Build**
- Reservation price and spread from the Avellaneda–Stoikov formulas, with
  inventory, risk aversion γ, volatility σ and the order-arrival decay k as
  inputs.
- Estimate σ and k for each chosen stock from the day's own data, and write
  down how.
- Round quotes to the tick ($0.01 for stocks priced at $1 or more in 2019).
- Requote when the book or the inventory changes, with a minimum interval, so
  you don't flood the exchange.
- Optional: shift the reservation price by an OFI-based forecast from stage 5.
  Run with and without it.
- Trade only during regular market hours (System Event messages mark the open
  and the close).

**Check:** unit tests of the formulas against hand calculations; your quotes
never cross the market or each other.

**Done when**
- [x] The strategy produces order intents (new, cancel, replace) for the
  chosen stocks over the whole day.

**Check questions**
- Derive the reservation price. What does γ do to the quotes?
- What did your paper find about Avellaneda–Stoikov on Binance, and what do
  you expect on Nasdaq stocks?

---

## Stage 7: Risk checks

**Goal:** no order leaves without passing every limit, and a kill switch built
on the safety budget from your safe RL research.

**Read:** your research notes on the "only affordable actions" result;
ARCHITECTURE.md, "Risk checks".

**Build**
- Pre-trade checks on every order: maximum order size, maximum position per
  stock, a price collar around the mid, maximum open orders, maximum messages
  per second.
- The loss-limit kill switch:
  - track equity (cash plus position marked at the mid) and its peak;
  - the remaining budget is z = L − (peak − equity), for a loss limit L;
  - allow an order only if its worst case keeps z ≥ 0. Define the worst case
    yourself (for example: the order fills and the price moves m ticks
    against you) and write down why you chose it;
  - when z is used up, cancel every order and stop quoting.
- Make the risk layer the only path to the exchange, so nothing can bypass it.

**Check**
- A unit test for every rule, at each limit and on either side of it.
- The kill switch fires once, and nothing is sent after it.

**Measure:** the cost of the checks themselves, which sit on the hot path.

**Done when**
- [x] Every rule is tested, the kill-switch test passes, and the cost is
  recorded.

**Check questions**
- How does this kill switch relate to your research result that allowing
  only affordable actions keeps the budget non-negative on every path? Where
  does the analogy stop?
- Why do risk checks belong on the hot path rather than on another thread?

---

## Stage 8: Exchange simulator

**Goal:** fill the strategy's orders against the replayed market,
deterministically, with realistic queue positions.

**Read:** Jane Street, "How to Build an Exchange"; Signals and Threads,
"State Machine Replication, and Why You Should Care"; ARCHITECTURE.md,
"Exchange simulator and sequencer".

**Build**
- A sequencer: every event (market message, order, acknowledgement, fill)
  gets one global sequence number, and everything is processed in that order.
- Order latency: an order reaches the simulated exchange a fixed delay after
  the market event that triggered it. Make the delay a parameter and try
  several values.
- Price-time matching of your orders against the replayed book:
  - your order joins the back of its price level;
  - the orders ahead of it leave through executions, cancels and deletes,
    which ITCH reports order by order, so you know your queue position
    exactly;
  - you're filled when executions reach your order, or when the market trades
    through your price.
- Acknowledgements, fills and cancel confirmations go back to the strategy
  through the sequencer.

**Check**
- Hand-built scenarios: queue position, partial fill, cancel before fill,
  trade-through, and an order that arrives after the price has moved.
- Determinism: run the whole day twice and compare a hash of the output log.
  The hashes must be identical.

**Done when**
- [x] Every scenario passes, and two full-day runs give the same hash.

**Check questions**
- What does a replay not capture? (Your own market impact, hidden liquidity,
  other traders reacting to you.)
- Why does a sequencer make the system deterministic, and why do exchanges
  and Jane Street's Concord platform use one?

---

## Stage 9: Threads and the hot path

**Goal:** the hot path on one thread, everything else off it, and a measured
reason for that split.

**Read:** Carl Cook's talk again; ARCHITECTURE.md, "Threads".

**Build**
- The hot path (feed handler → books → signals → strategy → risk checks →
  order out) on one thread that busy-polls, pinned to its own core on Linux.
- Your own single-producer single-consumer ring buffer, the simpler cousin of
  your MPMC queue, carrying log records and statistics to a second thread
  that writes them out.
- Prove there's no allocation on the hot path, for example by counting
  allocations in a test build.

**Check**
- ThreadSanitizer is clean.
- The threaded run produces exactly the same output log as the
  single-threaded run.

**Measure:** the cost of one queue hop: your SPSC queue, your MPMC queue and a
mutex-protected queue.

**Done when**
- [x] ThreadSanitizer is clean, the outputs are identical, and
  `results/queues.md` holds the three queue costs.

**Check questions**
- What do `memory_order_acquire` and `memory_order_release` guarantee in your
  ring buffer?
- What is false sharing, and where could it hit your queue?
- Why not give each stage its own thread?

---

## Stage 10: Measurement

**Goal:** honest latency numbers, for each stage and end to end.

**Build**
- Timestamps at each stage boundary (a calibrated cycle counter such as
  `rdtsc` on x86, or `std::chrono::steady_clock`), recorded into preallocated
  histograms.
- A report with the median, 99th and 99.9th percentile and maximum, per stage
  and end to end, plus the machine, compiler and flags.

**Method**
- Run on the Linux machine with the cores pinned and nothing else running.
  Warm up first, repeat the runs, and report the spread.
- Profile the slowest stage (`perf`), change one thing, measure again. Keep a
  log of what each change bought.

**Done when**
- [ ] `results/latency.md` holds the table, the machine, the spread across
  runs and the change log.
  (Table, machine and spread: yes, from the M1; the change log is in
  `results/book.md`. Open until the run on a pinned Linux x86 machine.)

**Check questions**
- Your numbers start when a message is already in memory. What does a firm's
  wire-to-wire number include that yours doesn't?
- Why report the 99.9th percentile and not only the average?

---

## Stage 11: Results and write-up

**Goal:** a project anyone can reproduce, with results you can defend.

**Build**
- Strategy results for the day: P&L, fills and inventory, with and without
  the kill switch, and with and without the OFI skew; how often the kill
  switch fired.
- The README: the question, the design, the results tables, the limitations,
  and how to reproduce everything from scratch.
- Limitations stated plainly: one day of sample data; a replay, not live
  trading; no market impact; latency measured from memory, not from the wire.

**Done when**
- [ ] A fresh clone, the data download and one command reproduce every number
  in the README.
  (`tools/reproduce.sh` regenerated every results file in one run on the M1;
  the fresh-clone test needs the first commit.)

---

## Stretch: market data over the network

- A replayer that sends the day's messages as MoldUDP64 packets (the framing
  Nasdaq uses for ITCH over multicast) on localhost, and a receiver that
  detects sequence gaps and recovers from them.
- Then measure the receive path too. Listen to Signals and Threads,
  "Multicast and the Markets", first.

(Built: `tools/network_demo.sh`, results in `results/network.md`. It uses
unicast UDP on localhost rather than multicast.)

---

## Sources

- [Nasdaq TotalView-ITCH 5.0 specification](https://nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf)
- [Nasdaq's sample ITCH files](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/)
- [Jane Street, "How to Build an Exchange"](https://blog.janestreet.com/how-to-build-an-exchange/) (Brian Nigito)
- [Signals and Threads, "Multicast and the Markets"](https://signalsandthreads.com/multicast-and-the-markets) (Brian Nigito)
- [Signals and Threads, "State Machine Replication, and Why You Should Care"](https://signalsandthreads.com/state-machine-replication-and-why-you-should-care/) (Doug Patti)
- [Carl Cook (Optiver), "When a Microsecond Is an Eternity: High Performance Trading Systems in C++", CppCon 2017](https://isocpp.org/blog/2018/09/cppcon-2017-when-a-microsecond-is-an-eternity-high-performance-trading-syst)
- Cont, Kukanov & Stoikov (2014), "The Price Impact of Order Book Events",
  *Journal of Financial Econometrics*.
- Avellaneda & Stoikov (2008), "High-frequency trading in a limit order book",
  *Quantitative Finance*.

Jane Street writes mostly OCaml. Take the architecture from its talks and the
C++ technique from C++ firms, and don't call this design "Jane Street-style
C++".
