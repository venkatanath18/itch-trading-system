# Stage 11: the strategy over the day

Written by `trade --report` from `results/raw/trade_runs.csv` and `results/raw/trade_stocks.csv`. Avellaneda-Stoikov market making with 100-share orders on the stocks in `results/calibration.md`, trading only between the Q and M system events. Fills come from the exchange simulator (queue position from the replayed order flow, no market impact). P&L marks any inventory left at 16:00 to the mid, as in the model; there are no fees or rebates.

- Machine: Apple M1, 8 cores, 8 GB RAM
- OS: Darwin 25.3.0 (arm64)
- Compiler: AppleClang 17.0.0.17000013, Release build, flags `-O3 -DNDEBUG`
- Timer: cntvct_el0, one tick = 41.6667 ns

| Run | Latency | Kill switch | OFI skew | γ scale | P&L $ | Fills | Shares | Max inventory | Max drawdown $ | Kill fired | Orders | Cancels | Risk rejects | Exchange rejects |
|---|---:|---|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|---:|
| base | 0 µs | on, L = $500 | off | 1 | -167.05 | 548 | 37836 | 260 | 910.54 | yes, 09:35:01.319524203 | 3068 | 0 | 47 | 88 |
| base-again | 0 µs | on, L = $500 | off | 1 | -167.05 | 548 | 37836 | 260 | 910.54 | yes, 09:35:01.319524203 | 3068 | 0 | 47 | 88 |
| base-threaded | 0 µs | on, L = $500 | off | 1 | -167.05 | 548 | 37836 | 260 | 910.54 | yes, 09:35:01.319524203 | 3068 | 0 | 47 | 88 |
| no-kill | 0 µs | off | off | 1 | -5839.74 | 11323 | 849413 | 485 | 6021.10 | no | 119403 | 6 | 0 | 1715 |
| ofi | 0 µs | on, L = $500 | on | 1 | 75.90 | 614 | 43699 | 239 | 1298.07 | yes, 09:35:10.512024058 | 3341 | 0 | 460 | 104 |
| no-kill-ofi | 0 µs | off | on | 1 | -5583.49 | 11809 | 893224 | 464 | 5767.19 | no | 124945 | 8 | 0 | 1813 |
| latency-10us | 10 µs | on, L = $500 | off | 1 | -2.70 | 558 | 38429 | 278 | 1414.52 | yes, 09:34:49.770054601 | 2860 | 0 | 15 | 102 |
| no-kill-latency-10us | 10 µs | off | off | 1 | -5734.20 | 11624 | 862134 | 499 | 5881.03 | no | 119179 | 7 | 0 | 2602 |
| latency-100us | 100 µs | on, L = $500 | off | 1 | -969.85 | 472 | 32115 | 339 | 2728.04 | yes, 09:34:43.298762995 | 2725 | 0 | 134 | 165 |
| no-kill-latency-100us | 100 µs | off | off | 1 | -6480.32 | 11613 | 835076 | 500 | 6697.68 | no | 119525 | 10 | 0 | 4306 |
| latency-1000us | 1000 µs | on, L = $500 | off | 1 | -450.76 | 456 | 31318 | 348 | 1210.06 | yes, 09:34:26.923580192 | 2521 | 0 | 306 | 244 |
| no-kill-latency-1000us | 1000 µs | off | off | 1 | -7589.12 | 12530 | 914842 | 497 | 7758.30 | no | 120308 | 9 | 0 | 6429 |
| no-kill-gamma-x0.25 | 0 µs | off | off | 0.25 | -4426.49 | 10234 | 747676 | 499 | 4607.10 | no | 116012 | 17 | 0 | 1048 |
| no-kill-gamma-x4 | 0 µs | off | off | 4 | -10202.54 | 15778 | 1201638 | 496 | 10413.32 | no | 122795 | 7 | 0 | 3327 |

The kill switch fired in 7 of the 7 runs that had it on.

## Determinism

The output log (every order, risk decision, exchange response, kill-switch event and minute mark, each with its sequencer number) is hashed with FNV-1a as it is written.

| Run | Log lines | Log hash |
|---|---:|---|
| base | 8310 | `7cb837c2034cdace` |
| base, run again | 8310 | `7cb837c2034cdace` |
| base, log written by a second thread through the SPSC queue | 8310 | `7cb837c2034cdace` |

Same hash on the second run: **yes**. Threaded log identical to the single-threaded one: **yes**.

### base, by stock

| Stock | P&L $ | Fills | Bought | Sold | Position at 16:00 | Max inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL | -48.12 | 219 | 6802 | 6689 | 113 | 260 |
| MSFT | -84.27 | 169 | 4946 | 4905 | 41 | 203 |
| INTC | -24.84 | 95 | 4602 | 4625 | -23 | 148 |
| CSCO | -9.81 | 65 | 2590 | 2677 | -87 | 123 |

### base-again, by stock

| Stock | P&L $ | Fills | Bought | Sold | Position at 16:00 | Max inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL | -48.12 | 219 | 6802 | 6689 | 113 | 260 |
| MSFT | -84.27 | 169 | 4946 | 4905 | 41 | 203 |
| INTC | -24.84 | 95 | 4602 | 4625 | -23 | 148 |
| CSCO | -9.81 | 65 | 2590 | 2677 | -87 | 123 |

### base-threaded, by stock

| Stock | P&L $ | Fills | Bought | Sold | Position at 16:00 | Max inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL | -48.12 | 219 | 6802 | 6689 | 113 | 260 |
| MSFT | -84.27 | 169 | 4946 | 4905 | 41 | 203 |
| INTC | -24.84 | 95 | 4602 | 4625 | -23 | 148 |
| CSCO | -9.81 | 65 | 2590 | 2677 | -87 | 123 |

### no-kill, by stock

| Stock | P&L $ | Fills | Bought | Sold | Position at 16:00 | Max inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL | -4652.12 | 4597 | 157966 | 157564 | 402 | 479 |
| MSFT | -490.76 | 2291 | 87834 | 87936 | -102 | 433 |
| INTC | -415.68 | 2842 | 113070 | 112955 | 115 | 485 |
| CSCO | -281.19 | 1593 | 66263 | 65825 | 438 | 462 |

### ofi, by stock

| Stock | P&L $ | Fills | Bought | Sold | Position at 16:00 | Max inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL | 250.21 | 245 | 8176 | 7982 | 194 | 239 |
| MSFT | -118.73 | 187 | 5682 | 5591 | 91 | 203 |
| INTC | -23.89 | 106 | 5193 | 5025 | 168 | 168 |
| CSCO | -31.69 | 76 | 3073 | 2977 | 96 | 123 |

### no-kill-ofi, by stock

| Stock | P&L $ | Fills | Bought | Sold | Position at 16:00 | Max inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL | -4263.18 | 4757 | 165658 | 165298 | 360 | 424 |
| MSFT | -619.98 | 2577 | 100178 | 100182 | -4 | 307 |
| INTC | -419.85 | 2843 | 113635 | 113499 | 136 | 464 |
| CSCO | -280.48 | 1632 | 67606 | 67168 | 438 | 462 |

### latency-10us, by stock

| Stock | P&L $ | Fills | Bought | Sold | Position at 16:00 | Max inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL | 174.27 | 215 | 7450 | 7246 | 204 | 278 |
| MSFT | -114.90 | 183 | 4922 | 4851 | 71 | 170 |
| INTC | -28.34 | 93 | 4502 | 4425 | 77 | 148 |
| CSCO | -33.73 | 67 | 2570 | 2463 | 107 | 109 |

### no-kill-latency-10us, by stock

| Stock | P&L $ | Fills | Bought | Sold | Position at 16:00 | Max inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL | -4472.82 | 4702 | 161486 | 161184 | 302 | 499 |
| MSFT | -578.90 | 2468 | 93570 | 93642 | -72 | 339 |
| INTC | -394.81 | 2812 | 109721 | 109588 | 133 | 455 |
| CSCO | -287.67 | 1642 | 66675 | 66268 | 407 | 493 |

### latency-100us, by stock

| Stock | P&L $ | Fills | Bought | Sold | Position at 16:00 | Max inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL | -841.66 | 196 | 6022 | 6185 | -163 | 339 |
| MSFT | -74.58 | 138 | 3648 | 3619 | 29 | 133 |
| INTC | -35.50 | 85 | 4100 | 4200 | -100 | 200 |
| CSCO | -18.11 | 53 | 2170 | 2171 | -1 | 104 |

### no-kill-latency-100us, by stock

| Stock | P&L $ | Fills | Bought | Sold | Position at 16:00 | Max inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL | -4751.20 | 4911 | 163611 | 163357 | 254 | 500 |
| MSFT | -889.47 | 2505 | 96984 | 96739 | 245 | 491 |
| INTC | -540.59 | 2782 | 102389 | 102107 | 282 | 482 |
| CSCO | -299.06 | 1415 | 55159 | 54730 | 429 | 471 |

### latency-1000us, by stock

| Stock | P&L $ | Fills | Bought | Sold | Position at 16:00 | Max inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL | -91.91 | 167 | 5700 | 5617 | 83 | 348 |
| MSFT | -285.63 | 162 | 4654 | 4408 | 246 | 246 |
| INTC | -50.00 | 68 | 3200 | 3200 | 0 | 200 |
| CSCO | -23.23 | 59 | 2267 | 2272 | -5 | 105 |

### no-kill-latency-1000us, by stock

| Stock | P&L $ | Fills | Bought | Sold | Position at 16:00 | Max inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL | -5514.84 | 5366 | 182625 | 182478 | 147 | 489 |
| MSFT | -1196.26 | 3120 | 122021 | 121819 | 202 | 448 |
| INTC | -573.08 | 2657 | 99681 | 99291 | 390 | 490 |
| CSCO | -304.94 | 1387 | 53665 | 53262 | 403 | 497 |

### no-kill-gamma-x0.25, by stock

| Stock | P&L $ | Fills | Bought | Sold | Position at 16:00 | Max inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL | -3568.43 | 3767 | 128648 | 128242 | 406 | 499 |
| MSFT | -55.69 | 1377 | 50267 | 50337 | -70 | 484 |
| INTC | -451.46 | 3286 | 122786 | 122681 | 105 | 492 |
| CSCO | -350.90 | 1804 | 72560 | 72155 | 405 | 495 |

### no-kill-gamma-x4, by stock

| Stock | P&L $ | Fills | Bought | Sold | Position at 16:00 | Max inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL | -8371.53 | 7466 | 267230 | 266909 | 321 | 496 |
| MSFT | -1204.36 | 3989 | 157128 | 156990 | 138 | 299 |
| INTC | -378.89 | 2749 | 111519 | 111330 | 189 | 489 |
| CSCO | -247.75 | 1574 | 65495 | 65037 | 458 | 458 |

