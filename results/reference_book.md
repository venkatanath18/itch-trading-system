# Stage 3: the reference book

Written by `book_check`. The whole day replayed through `ReferenceBook` (`std::unordered_map` + `std::map` + `std::list`), checking after every message.

- Machine: Apple M1, 8 cores, 8 GB RAM
- OS: Darwin 25.3.0 (arm64)
- Compiler: AppleClang 17.0.0.17000013, Release build, flags `-O3 -DNDEBUG`
- Timer: cntvct_el0, one tick = 41.6667 ns

## Invariants

| Check | Result |
|---|---|
| Messages replayed | 268,744,780 |
| Book messages checked (A F E C X D U) | 263,241,937 |
| After each one: the touched levels are non-empty, each level's total equals the sum of its orders, no order has zero shares, the order map agrees with the levels | held every time (0 violations) |
| Full check of every level of every stock, every 20 million messages and at the end | 14 full checks |
| Orders left at the end of the day | 0 |
| Book errors (unknown order, duplicate reference, over-execution) | 0 |
| Run time with all the checks | 181.4 s |

## Best bid below best ask during market hours

An episode starts when a message leaves a stock's best bid at or above its best ask between the Q and M events, and ends when a later message uncrosses it.

| Episodes | Count | Explanation |
|---|---:|---|
| Starting in the first second after the open | 2 | Orders entered before 9:30 can cross; the opening cross executes them (C messages, printable N) within milliseconds of the Q event |
| Stock halted, paused or quotation-only (H message state not T) | 31 | No continuous matching during a halt, so the book can cross until the reopening cross |
| Any other time | 0 | listed below |

## Where in the book the activity is

Distance of each new order (A, F and the new half of U) from the best price on its own side just before it arrived:

| Ticks from the best on its side | Orders added | Share | Cumulative |
|---|---:|---:|---:|
| better than the best (a new best) | 15,578,694 | 11.11% | 11.11% |
| 0 | 50,465,583 | 35.98% | 47.08% |
| 1 | 10,535,791 | 7.51% | 54.59% |
| 2 | 6,044,643 | 4.31% | 58.90% |
| 3 | 4,667,958 | 3.33% | 62.23% |
| 4 | 4,029,446 | 2.87% | 65.10% |
| 5 to 9 | 16,148,858 | 11.51% | 76.62% |
| 10 to 19 | 10,176,801 | 7.26% | 83.87% |
| 20 to 49 | 5,662,934 | 4.04% | 87.91% |
| 50 to 99 | 2,469,841 | 1.76% | 89.67% |
| 100 to 199 | 2,304,154 | 1.64% | 91.31% |
| 200 to 499 | 4,080,050 | 2.91% | 94.22% |
| 500 to 999 | 4,014,940 | 2.86% | 97.08% |
| 1000 or more | 4,000,210 | 2.85% | 99.94% |
| side was empty | 90,620 | 0.06% | 100.00% |

Distance of each order that left or shrank (E, C, X, D and the old half of U):

| Ticks from the best on its side | Orders removed or reduced | Share | Cumulative |
|---|---:|---:|---:|
| better than the best (a new best) | 0 | 0.00% | 0.00% |
| 0 | 59,706,225 | 41.29% | 41.29% |
| 1 | 18,056,395 | 12.49% | 53.77% |
| 2 | 7,293,005 | 5.04% | 58.82% |
| 3 | 5,077,752 | 3.51% | 62.33% |
| 4 | 4,124,006 | 2.85% | 65.18% |
| 5 to 9 | 17,100,193 | 11.83% | 77.01% |
| 10 to 19 | 10,423,559 | 7.21% | 84.21% |
| 20 to 49 | 5,808,580 | 4.02% | 88.23% |
| 50 to 99 | 2,522,893 | 1.74% | 89.97% |
| 100 to 199 | 2,301,100 | 1.59% | 91.57% |
| 200 to 499 | 4,182,701 | 2.89% | 94.46% |
| 500 to 999 | 3,984,841 | 2.76% | 97.21% |
| 1000 or more | 4,029,231 | 2.79% | 100.00% |
| side was empty | 0 | 0.00% | 100.00% |

## A snapshot

The AAPL book at 10:00:00.000042368, top five levels:

| Bid orders | Bid shares | Bid | Ask | Ask shares | Ask orders |
|---:|---:|---:|---:|---:|---:|
| 1 | 100 | 286.8300 | 286.8600 | 1 | 1 |
| 1 | 100 | 286.8200 | 286.8700 | 100 | 1 |
| 2 | 200 | 286.8100 | 286.8900 | 100 | 1 |
| 1 | 100 | 286.8000 | 286.9000 | 100 | 1 |
| 3 | 400 | 286.7900 | 286.9100 | 100 | 1 |

