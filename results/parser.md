# Stage 2: the feed handler

Written by `parse_bench`. Decoding means: read the 2-byte length, switch on the type, read every field with big-endian loads into a small struct and call the handler; types the books don't need are skipped by their length.

- Machine: Apple M1, 8 cores, 8 GB RAM
- OS: Darwin 25.3.0 (arm64)
- Compiler: AppleClang 17.0.0.17000013, Release build, flags `-O3 -DNDEBUG`
- Timer: cntvct_el0, one tick = 41.6667 ns

## Every message decodes

| Check | Result |
|---|---|
| Messages decoded | 268,744,780 |
| Same as stage 1 (`day_summary`) | yes |
| Counts by type from decoding equal counts from the framing walk, and three decoding runs agree | yes |

## Throughput over the whole day

| Pass | Seconds | Messages per second | ns per message |
|---|---:|---:|---:|
| Framing walk only (length + type byte), parse time only | 0.84 | 319,865,766 | 3.13 |
| **Full decode, parse time only** (median of 3 runs) | **2.21** | **121,837,232** | **8.21** |
| Full decode, spread over the 3 runs | 2.19 to 2.24 | | 8.16 to 8.33 |
| Reading the file in 256 MB chunks (`read`), alongside the above | 2.96 | | |
| Full decode straight from `mmap`, page faults included | 18.96 | 14,171,147 | 70.57 |

The day is 8.25 GB, more than this machine's 8 GB of memory, so it can't stay in the page cache. Through `mmap` every 16 KB page is a page fault while the parser waits; reading 256 MB chunks with `read` into one reused buffer is several times faster and keeps the parser from ever waiting on the disk. The later stages use the chunked reader.

## The switch and the branch predictor

The same ten million messages (from 10:00), parsed from memory in three orders. The work is identical; only the predictability of the `switch` on the type changes. Median of 5 runs.

| Order | ns per message |
|---|---:|
| As in the feed | 8.41 |
| Shuffled at random | 8.64 |
| Sorted by type (all A, then all C, ...) | 4.54 |

The three runs do the same work on the same bytes; only the order of the types differs, so the gaps come from the branch predictor guessing where the `switch` jumps. Sorted, the guess is almost always right. Shuffled, the next type is a draw from the mix (A 44%, D 43%, U 8%, ...), so many guesses miss: 4.11 ns per message more than sorted. The feed's own order costs 3.87 ns more than sorted, almost as much: consecutive messages are mostly about different stocks and orders, so the next type is close to a random draw. Sorting also lets the decoding code that follows the jump stay hot, so the gap is an upper bound on the mispredicts' share.

(checksum 29012)
