# Stage 1: the day

Written by `day_summary`.

- File: `12302019.NASDAQ_ITCH50`, 8,251,407,909 bytes decompressed
- MD5 of the `.gz` download: `23514ef98821448a3f8f4c5b4095f3f5` (Nasdaq's `.md5sum` file returned 404; gzip's CRC-32 check passed while decompressing)
- Machine: Apple M1, 8 cores, 8 GB RAM
- OS: Darwin 25.3.0 (arm64)
- Compiler: AppleClang 17.0.0.17000013, Release build, flags `-O3 -DNDEBUG`
- Timer: cntvct_el0, one tick = 41.6667 ns

## Checks

| Check | Result |
|---|---|
| First message is a System Event | yes (type `S`, event code `O`) |
| Framing reaches the end of the file | ok, 8,251,407,909 of 8,251,407,909 bytes |
| Counts by type add up to the total | yes (268,744,780 = 268,744,780) |
| A second run gives the same counts | yes |
| Every message decodes | ok, 268,744,780 messages |

## Messages by type

| Type | Name | Count | Share |
|---|---|---:|---:|
| `A` | Add Order | 117,145,568 | 43.59% |
| `D` | Order Delete | 114,360,997 | 42.55% |
| `U` | Order Replace | 21,639,067 | 8.05% |
| `E` | Order Executed | 5,722,824 | 2.13% |
| `I` | NOII | 4,024,315 | 1.50% |
| `X` | Order Cancel | 2,787,676 | 1.04% |
| `F` | Add Order with MPID | 1,485,888 | 0.55% |
| `P` | Trade (non-cross) | 1,218,602 | 0.45% |
| `L` | Market Participant Position | 215,161 | 0.08% |
| `C` | Order Executed with Price | 99,917 | 0.04% |
| `Q` | Cross Trade | 17,836 | 0.01% |
| `Y` | Reg SHO Restriction | 9,013 | 0.00% |
| `H` | Stock Trading Action | 8,966 | 0.00% |
| `R` | Stock Directory | 8,906 | 0.00% |
| `J` | LULD Auction Collar | 34 | 0.00% |
| `S` | System Event | 6 | 0.00% |
| `K` | IPO Quoting Period Update | 3 | 0.00% |
| `V` | MWCB Decline Level | 1 | 0.00% |
| | **Total** | **268,744,780** | |

## Run time

| Pass | Seconds | Messages per second |
|---|---:|---:|
| Walk the framing and count by type (run 1) | 22.40 | 11,995,868 |
| Walk the framing and count by type (run 2) | 24.06 | 11,168,760 |
| Decode everything and track live orders in `std::unordered_map` | 84.82 | 3,168,522 |

The file is larger than this machine's memory, so the walks read it from the SSD.

## Session

| Event | Code | Time |
|---|---|---|
| Start of messages | `O` | 03:04:32.057543747 |
| Start of system hours | `S` | 04:00:00.000198145 |
| Start of market hours | `Q` | 09:30:00.000075566 |
| End of market hours | `M` | 16:00:00.000071785 |
| End of system hours | `E` | 20:00:00.000019686 |
| End of messages | `C` | 20:05:00.000040070 |

## What the later stages need

| Quantity | Value |
|---|---:|
| Stock Directory messages (stocks) | 8,906 |
| Largest stock locate code | 8906 |
| Stocks with at least one order | 8,892 |
| **Peak number of live orders** | **1,924,078** at 15:55:58.182031429 |
| Live orders at the end of the day | 0 |
| Order reference numbers used (adds + replacements) | 140,270,523 |
| Smallest / largest reference number | 42 / 308,631,072 |
| Density: numbers used / (largest - smallest + 1) | 0.454 |
| New reference number not above all earlier ones | 102,516,294 |
| Reference number added twice while live | 0 |
| E / C / X / D / U naming an unknown order | 0 / 0 / 0 / 0 / 0 |
| Executions or cancels larger than the order | 0 |
| Replace whose header locate differs from the order's | 0 |
| Orders priced at $1 or more but not in whole cents | 0 |
| Orders with zero shares | 0 |

### Per stock (stocks with orders)

| Per stock | Median | 99th percentile | Max |
|---|---:|---:|---:|
| Peak live orders | 120 | 1905 | 37068 |
| Peak price levels on one side | 36 | 377 | 4227 |
| Range of all order prices, in ticks | 19,999,998 | 19,999,998 | 19,999,998 |
| Range of execution prices in market hours, in ticks | 27 | 829 | 5,488 |

6,719 of 8,892 stocks have orders both at $0.01 or less and at $100,000 or more: stub quotes, which market makers leave far from the market. So a flat array over a stock's whole price range is out of the question, while the prices that actually trade span a few hundred ticks.

Sum over stocks of the peak level count of each side: 883,240.

### Most active stocks, and the ones used later

| Symbol | Adds | Traded low | Traded high | Peak live orders | Peak bid levels | Peak ask levels |
|---|---:|---:|---:|---:|---:|---:|
| QQQ | 1,221,096 | 211.1600 | 213.6300 | 8,847 | 2164 | 468 |
| SPY | 1,109,146 | 320.5500 | 323.1000 | 1,949 | 544 | 239 |
| IWM | 1,004,648 | 164.5800 | 166.2000 | 434 | 85 | 89 |
| TQQQ | 874,084 | 84.8000 | 87.8600 | 4,348 | 1104 | 417 |
| AAPL | 795,712 | 285.2300 | 292.6900 | 27,110 | 4227 | 1160 |
| URTY | 790,501 | 83.7200 | 85.9300 | 133 | 61 | 56 |
| AMD | 733,505 | 44.6600 | 46.1700 | 11,614 | 1355 | 673 |
| MSFT | 630,899 | 156.7300 | 159.0100 | 23,005 | 2969 | 871 |
| TNA | 512,422 | 70.5500 | 72.6400 | 655 | 307 | 217 |
| DIA | 503,663 | 284.1300 | 286.4100 | 624 | 263 | 109 |
| INTC | 391,814 | 59.3900 | 60.0500 | 8,609 | 1152 | 578 |
| CSCO | 284,929 | 47.2900 | 47.8800 | 13,408 | 939 | 911 |
| AMZN | 287,475 | 1840.5300 | 1884.0000 | 37,068 | 3060 | 2570 |
