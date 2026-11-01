# Stage 4: three order books on the same day

Written by `book_bench --report` from `results/raw/book_bench.csv`. Each book replays the whole day in its own process. Each sample is one book message (A F E C X D U), timed from after decoding to after the book call; parsing and file reads are outside the samples. A book run several times shows the median of its runs, and the range of the mean across them in brackets.

- Machine: Apple M1, 8 cores, 8 GB RAM
- OS: Darwin 25.3.0 (arm64)
- Compiler: AppleClang 17.0.0.17000013, Release build, flags `-O3 -DNDEBUG`
- Timer: cntvct_el0, one tick = 41.6667 ns
- Timer overhead (back-to-back reads, included in every sample): 8.8 ns on average

| Book | Runs | Mean ns | Median ns | 99% ns | 99.9% ns | Max µs | Replay s | of which file reads s | Peak memory MB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Earlier book (`shared_ptr` per order, `std::map<double, ...>`) | 1 | 500.4 | 417 | 1750 | 2917 | 7352.4 | 140.5 | 3.0 | 664 |
| Reference book (`unordered_map` + `map` + `list`) | 1 | 398.6 | 292 | 1458 | 2375 | 108624.0 | 113.6 | 3.0 | 494 |
| Fast: hash table + sorted vector, linear search (first version) | 3 | 174.3 (171.0-178.4) | 125 | 625 | 1125 | 6701.8 | 54.5 | 3.0 | 527 |
| Fast: flat array + sorted vector, linear search | 3 | 98.0 (96.6-99.0) | 42 | 500 | 1333 | 1521.6 | 34.3 | 2.9 | 1501 |
| Fast: hash table + sorted vector | 3 | 159.3 (157.0-161.6) | 125 | 583 | 1042 | 1781.6 | 50.4 | 3.0 | 527 |
| Fast: flat array + sorted vector | 3 | 99.7 (97.9-111.1) | 42 | 500 | 1375 | 1779.8 | 34.8 | 3.0 | 1398 |
| Fast: hash table (ref mod size) + sorted vector | 3 | 114.7 (112.8-115.8) | 42 | 542 | 1708 | 1951.4 | 38.5 | 3.0 | 527 |
| Fast: hash table + tick array (1024 ticks) | 3 | 196.2 (194.8-196.3) | 167 | 750 | 1250 | 5579.9 | 60.0 | 3.0 | 601 |
| Fast: hash table + tick array (256 ticks) | 3 | 197.9 (194.8-200.4) | 167 | 750 | 1250 | 4308.6 | 60.5 | 3.0 | 547 |

263,241,937 book messages out of 268,744,780 in each run. Percentiles come from a histogram with 64 buckets per power of two.

On Apple Silicon the counter ticks every 41.7 ns, so the percentiles here are whole numbers of ticks (0, 42, 83, ... ns) and say little below 100 ns. The mean is still meaningful, because the rounding errors average out over 263 million samples. Take the final percentiles on a Linux x86 machine, where `rdtsc` resolves well under 1 ns.

## Mean time per message, as ratios

| Comparison | Ratio |
|---|---:|
| Earlier book / reference book | 1.26x |
| Reference book / fast book (hash + sorted vector) | 2.50x |
| Earlier book / fast book (hash + sorted vector) | 3.14x |
| Linear search / linear then binary search (hash table) | 1.09x |
| Hash table / flat array (both with sorted vectors) | 1.60x |
| Hash table (ref mod size) / flat array | 1.15x |
| Sorted vector / tick array of 1024 (both with hash table) | 0.81x |
| Tick array of 256 / tick array of 1024 | 1.01x |

## Change log

The book is the slowest stage by far (`results/latency.md`), so the changes went there, one at a time, each measured over the whole day on this machine. Mean ns per book message:

| Step | Change | Mean |
|---|---|---:|
| 0 | First version: hash table, sorted vector searched linearly from the best price | 174.3 ns |
| 1 | Sorted vector: linear for 16 levels, then binary search (stub quotes sit thousands of levels deep) | 159.3 ns |
| 2 | Order lookup: flat array indexed by reference number instead of the hash table | 99.7 ns |
| tried | Hash table with slot = ref mod size, to keep neighbouring refs together | 114.7 ns |
| tried | Tick array of 1024 ticks around the best instead of the sorted vector | 196.2 ns |
| tried | Tick array of 256 ticks | 197.9 ns |
| check | Flat array with the linear search, to separate the two changes | 98.0 ns |
