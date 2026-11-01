# Stage 7: the cost of the risk checks

Written by `risk_bench`. A risk gate with four stocks, a position of 300 shares and six open orders. 2,000,000 calls per row, timed in batches of 1000, repeated in 5 rounds on a fresh gate. Each cell is the median over the rounds of the batches' median, 99th percentile and mean time per call; the range of the median across the rounds is in brackets.

- Machine: Apple M1, 8 cores, 8 GB RAM
- OS: Darwin 25.3.0 (arm64)
- Compiler: AppleClang 17.0.0.17000013, Release build, flags `-O3 -DNDEBUG`
- Timer: cntvct_el0, one tick = 41.6667 ns

| Call | Median ns | 99% of batches ns | Mean ns |
|---|---:|---:|---:|
| `submit`, a replace that passes every check and goes to the exchange queue | 16.2 (16.1-26.0) | 17.1 | 16.2 |
| `submit`, an order refused by the first check (size) | 2.1 (2.1-3.1) | 2.2 | 2.1 |
| `mark`: equity over four stocks, peak, budget, kill-switch test | 4.3 (4.3-5.7) | 4.5 | 4.3 |

Accepted 10,000,000 of the replaces and refused 10,000,000 of the oversized orders (all of them, as expected).

The same checks are also timed inside the full system (stage 10, `results/latency.md`, the risk stage), where the gate's state is colder in the cache.
