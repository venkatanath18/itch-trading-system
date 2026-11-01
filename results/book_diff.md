# Stage 4: fast books against the reference book

Written by `book_diff`. One replay of the whole day drives the reference book and four fast books. After **every** book message (A F E C X D U) the touched stock's top 5 levels on both sides (price, shares, order count) are compared; a message only changes its own stock's book. Every 20 million book messages and at the end of the day, every level of every stock and the order counts are compared too.

- Machine: Apple M1, 8 cores, 8 GB RAM
- OS: Darwin 25.3.0 (arm64)
- Compiler: AppleClang 17.0.0.17000013, Release build, flags `-O3 -DNDEBUG`
- Timer: cntvct_el0, one tick = 41.6667 ns

| | Value |
|---|---:|
| Messages | 268,744,780 |
| Book messages, each followed by a comparison | 263,241,937 |
| Full comparisons (all levels, all stocks) | 14 |
| Run time | 479.7 s |

| Fast book | Differences | First difference | Book errors | Level-vector growths |
|---|---:|---|---:|---:|
| hash table + sorted vector | 0 | none | 0 | 0 |
| flat array + sorted vector | 0 | none | 0 | 0 |
| hash table + tick array (1024 ticks) | 0 | none | 0 | 0 |
| hash table + tick array (256 ticks) | 0 | none | 0 | 0 |

Reference book errors: 0. Orders left at the end: reference 0, fast 0.
