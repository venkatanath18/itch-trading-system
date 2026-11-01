# Stage 6: strategy inputs, estimated from the day

Written by `calibrate`, from 9:45 to 15:45 (21600 one-second steps).

- Machine: Apple M1, 8 cores, 8 GB RAM
- OS: Darwin 25.3.0 (arm64)
- Compiler: AppleClang 17.0.0.17000013, Release build, flags `-O3 -DNDEBUG`
- Timer: cntvct_el0, one tick = 41.6667 ns

| Stock | Mid at 12:45 | Mean spread (ticks) | σ ($/√s) | σ over 6.5 h | k (per $) | A (per s) | Fit R² | OFI β ($ per 1,000 shares) | γ (per $) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| AAPL | 291.53 | 2.90 | 0.01869 | 0.98% | 58.9 | 0.3512 | 0.997 | 0.001232 | 2.446e-05 |
| MSFT | 158.03 | 1.29 | 0.006321 | 0.61% | 144.0 | 0.2323 | 0.985 | 0.0003511 | 0.0002139 |
| INTC | 59.78 | 1.08 | 0.003014 | 0.77% | 305.7 | 0.25 | 1.000 | 5.876e-05 | 0.0009409 |
| CSCO | 47.66 | 1.08 | 0.002314 | 0.74% | 320.9 | 0.1853 | 1.000 | 3.143e-05 | 0.001596 |

## How often trades reach a distance from the mid within one second

Pooled over the bid and ask sides. The fit of k and A uses the distances with at least 20 hits.

| Stock | 0.5 ticks | 1.5 ticks | 2.5 ticks | 3.5 ticks | 4.5 ticks | 5.5 ticks | 6.5 ticks | 7.5 ticks |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| AAPL | 24.45% | 14.79% | 7.12% | 3.95% | 2.31% | 1.36% | 0.76% | 0.47% |
| MSFT | 14.67% | 2.07% | 0.49% | 0.14% | 0.05% | 0.02% | 0.01% | 0.00% |
| INTC | 5.28% | 0.25% | 0.02% | 0.00% | 0.00% | 0.00% | 0.00% | 0.00% |
| CSCO | 3.66% | 0.15% | 0.01% | 0.00% | 0.00% | 0.00% | 0.00% | 0.00% |

## What the model then quotes at mid-day

Half the A-S spread and the shift of the reservation price for 100 shares held, both at 12:45 (3.25 hours left), in ticks.

| Stock | 2/k in ticks | Half-spread | Shift per 100 shares |
|---|---:|---:|---:|
| AAPL | 3.40 | 1.70 | 1.00 |
| MSFT | 1.39 | 0.70 | 1.00 |
| INTC | 0.65 | 0.33 | 1.00 |
| CSCO | 0.62 | 0.32 | 1.00 |

These use the same day the strategy then trades on, which is look-ahead: a live system would estimate them on earlier days. With one day of data, that is a stated limitation.
