# Stage 10: latency, stage by stage

Written by `trade --latency-report` from `results/raw/latency.csv`. The whole system replays the day 3 times with a timestamp at every stage boundary, recorded into preallocated histograms. Each cell is the median over the runs, with the range across runs in brackets; nanoseconds. The time a message waits in memory before t0 isn't included (see below).

- Machine: Apple M1, 8 cores, 8 GB RAM
- OS: Darwin 25.3.0 (arm64)
- Compiler: AppleClang 17.0.0.17000013, Release build, flags `-O3 -DNDEBUG`
- Timer: cntvct_el0, one tick = 41.6667 ns
- Timer overhead: two back-to-back counter reads differ by 19.3 ns on average; every stage below includes about that much

| Stage | Samples per run | Median | 99% | 99.9% | Max | Mean |
|---|---:|---:|---:|---:|---:|---:|
| t0-t1: decode + book update (book messages) | 257,862,662 | 42 (42-83) | 542 (500-542) | 2583 (1292-2750) | 2249250 (1852790-3475960) | 114 (110-118) |
| t1-t2: top of book + OFI (book messages) | 257,862,662 | 42 (42-42) | 250 (250-250) | 417 (417-458) | 1019750 (580000-1025920) | 39 (39-40) |
| t2-t3: strategy decision (top-of-book changes of traded stocks) | 1,787,214 | 42 (42-42) | 83 (83-83) | 167 (167-208) | 607375 (447125-1180500) | 40 (39-41) |
| t3-t4: risk checks (per order intent) | 3,115 | 42 (42-42) | 125 (125-167) | 250 (250-417) | 583 (375-2708) | 46 (44-48) |
| t4-t5: hand to the exchange (per order sent) | 3,068 | 42 (42-42) | 83 (42-83) | 4375 (2750-5250) | 7625 (5333-8000) | 40 (37-40) |
| **t0-t5: end to end, messages that led to an order** | 2,040 | 333 (333-333) | 667 (625-1167) | 6542 (3042-8375) | 607833 (447583-1181040) | 658 (578-987) |
| every market message, start to end | 262,048,061 | 125 (125-125) | 708 (667-708) | 2708 (1542-2917) | 2249790 (1853000-3477540) | 168 (163-173) |

Whole-day replay of the full system: 32.5 s without the timestamps (run `base`), 52.8 s with them (run `measure-1`). The difference is the cost of reading the counter six times per message.

On this machine the counter ticks every 41.7 ns, so every percentile is a whole number of ticks (0, 42, 83, 125, ... ns) and stages shorter than a tick mostly read 0; the means average out the rounding. These are not the final latency numbers: those need a Linux x86 machine with pinned, isolated cores and `rdtsc` (one command, see the README).

## What these numbers leave out

t0 is taken when a message is already in memory. A firm's wire-to-wire number also includes the network card, the kernel or kernel bypass stack, reading the packet, and on the way out, encoding the order and sending it, typically several microseconds more. This system also replays from a file in large chunks, so it never waits for the next packet.
