# Stage 5: order-flow imbalance inside the system

Written by `analysis/ofi_check.py` from the files `signals_export` writes. Python 3.13.12, numpy 2.5.4, polars 2.0.0.

## The C++ OFI against the order-flow study's Python code

One stock's top-of-book changes from the reference book: AAPL, 87,012 changes between 09:59:50 and 11:00:00. The C++ values come from the `Signals` module the strategy uses; the Python values from `ofi_contributions` in `p1-order-flow/src/orderflow/ingest.py`, imported unchanged.

| Check | Result |
|---|---|
| e_n for every change: C++ equals Python exactly | yes |
| OFI over the 1-second window (10 buckets of 100 ms), every change from 10:00 | yes |
| OFI over the 10-second window (10 buckets of 1 s), every change from 10:00 | yes |
| Sum of e_n over the hour | C++ 134,044, Python 134,044 |
| Changes with an empty side (e_n = 0 by rule) | 0 |

## Does OFI explain equity price moves too?

Each stock's market hours from 9:45 to 15:45 (away from the opening and closing crosses), cut into 10-second intervals. In each: the change of the mid-price in ticks, the summed OFI in shares, and the average depth at the best prices, (bid size + ask size) / 2. As in the study and in Cont, Kukanov & Stoikov (2014), `normalised` OFI is OFI / depth. t-statistics use Newey-West (HAC, 6 lags) standard errors.

| Stock | Intervals | Mid moved in | Median depth (shares) | β (ticks per 1,000 shares) | t | R² raw OFI | R² normalised | Median R² of 30-min windows |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| AAPL | 2,160 | 96% | 200 | 1.440 | 18.0 | 0.71 | 0.86 | 0.87 |
| MSFT | 2,160 | 76% | 602 | 0.524 | 27.8 | 0.77 | 0.88 | 0.87 |
| INTC | 2,160 | 37% | 1,762 | 0.112 | 3.2 | 0.49 | 0.84 | 0.81 |
| CSCO | 2,160 | 26% | 3,019 | 0.085 | 23.4 | 0.67 | 0.81 | 0.77 |
| AMZN | 2,160 | 91% | 52 | 50.770 | 15.5 | 0.52 | 0.43 | 0.50 |

For comparison: Cont, Kukanov & Stoikov report an average R² of about 65% for 10-second intervals within half-hour windows on 50 US stocks (2010); the order-flow study found 53-59% for Binance BTC and ETH perpetual futures (out of sample, 10 s).

### Forecasting the next interval

The next interval's mid change regressed on this interval's normalised OFI: fitted on 9:45-12:15, tested on 12:15-15:45. Out-of-sample R² is against predicting no change (negative = worse than that).

| Stock | 10 s in sample | 10 s out of sample | 1 s in sample | 1 s out of sample |
|---|---:|---:|---:|---:|
| AAPL | 0.12% | -0.21% | 0.40% | 0.62% |
| MSFT | 0.25% | 0.16% | 0.53% | 0.31% |
| INTC | 0.00% | -0.19% | 0.32% | 0.21% |
| CSCO | 0.16% | 0.72% | 0.22% | -0.03% |
| AMZN | 2.45% | -3.36% | 0.40% | -1.48% |

