"""Stage 5: check the C++ order-flow imbalance against the order-flow study's
own Python code, then regress mid-price changes on OFI for a few stocks.

Run with the order-flow study's environment, from the p6 folder:
    ../p1-order-flow/.venv/bin/python analysis/ofi_check.py data/derived/12302019 \
        --p1 ../p1-order-flow --out results/ofi_check.md

Inputs come from signals_export: <prefix>.tob_check.csv and
<prefix>.intervals_1s.csv / .intervals_10s.csv.
"""
import argparse
import platform
import sys
from pathlib import Path

import numpy as np
import polars as pl
import statsmodels.api as sm

OPEN_NS = (9 * 3600 + 30 * 60) * 10**9


def load_p1(p1_path):
    sys.path.insert(0, str(Path(p1_path) / "src"))
    from orderflow.ingest import ofi_contributions  # the study's code, unchanged
    return ofi_contributions


def python_ofi(rows, start, ofi_contributions):
    """e for every change row, computed by the study's function. It is called on
    each run of consecutive two-sided rows; the first row of a run is compared
    with the row before it if that one was two-sided, otherwise it gets 0, like
    the first row of a file in the study."""
    bid = rows["bid"].to_numpy().astype(np.float64)
    bid_qty = rows["bid_qty"].to_numpy().astype(np.float64)
    ask = rows["ask"].to_numpy().astype(np.float64)
    ask_qty = rows["ask_qty"].to_numpy().astype(np.float64)
    two_sided = (bid_qty > 0) & (ask_qty > 0)

    start_two_sided = start["bid_qty"] > 0 and start["ask_qty"] > 0
    e = np.zeros(len(rows))
    i = 0
    while i < len(rows):
        if not two_sided[i]:
            i += 1
            continue
        j = i
        while j + 1 < len(rows) and two_sided[j + 1]:
            j += 1
        previous = None
        if i == 0 and start_two_sided:
            previous = (start["bid"], start["bid_qty"], start["ask"], start["ask_qty"])
        e[i:j + 1] = ofi_contributions(bid[i:j + 1], bid_qty[i:j + 1], ask[i:j + 1], ask_qty[i:j + 1], previous)
        i = j + 1
    return e


def bucketed_window(ts, e, width_ns, buckets):
    """Sum of e over the current bucket and the buckets-1 before it, up to each row."""
    ids = ts // width_ns
    cumulative = np.cumsum(e)
    first = np.searchsorted(ids, ids - buckets + 1, side="left")
    before = np.where(first > 0, cumulative[first - 1], 0)
    return cumulative - before


def regression_rows(intervals, symbol):
    """Market hours from 9:45 to 15:45, away from the opening and closing crosses."""
    d = intervals.filter(pl.col("symbol") == symbol).sort("start_ns")
    d = d.with_columns(pl.col("dmid_ticks").shift(-1).alias("dmid_next"))
    lo, hi = OPEN_NS + 15 * 60 * 10**9, OPEN_NS + 375 * 60 * 10**9
    d = d.filter((pl.col("start_ns") >= lo) & (pl.col("start_ns") < hi) & (pl.col("avg_depth") > 0))
    return d.drop_nulls(["dmid_next"])


def ols(y, x, hac_lags=None):
    X = sm.add_constant(np.asarray(x, dtype=float))
    model = sm.OLS(np.asarray(y, dtype=float), X)
    if hac_lags is None:
        return model.fit()
    return model.fit(cov_type="HAC", cov_kwds={"maxlags": hac_lags})


def r2_oos(y, y_hat):
    """1 - SSE / sum y^2: compared with predicting no change, as in the study."""
    return 1.0 - np.sum((y - y_hat) ** 2) / np.sum(y ** 2)


def stock_results(intervals_10s, intervals_1s, symbol):
    d = regression_rows(intervals_10s, symbol)
    y = d["dmid_ticks"].to_numpy()
    ofi = d["ofi"].to_numpy().astype(float)
    norm = ofi / d["avg_depth"].to_numpy()
    raw_fit = ols(y, ofi, hac_lags=6)
    norm_fit = ols(y, norm, hac_lags=6)

    # Cont, Kukanov & Stoikov estimate within half-hour windows
    windows = []
    start = d["start_ns"].to_numpy()
    for w in range(12):
        lo = OPEN_NS + (15 + 30 * w) * 60 * 10**9
        mask = (start >= lo) & (start < lo + 30 * 60 * 10**9)
        if mask.sum() > 30 and np.std(norm[mask]) > 0:
            windows.append(ols(y[mask], norm[mask]).rsquared)

    # forecasting the next interval: fit on the morning, test on the afternoon
    def forecast(frame):
        x = (frame["ofi"].to_numpy() / frame["avg_depth"].to_numpy()).astype(float)
        target = frame["dmid_next"].to_numpy()
        noon = OPEN_NS + 150 * 60 * 10**9
        train = frame["start_ns"].to_numpy() < noon
        fit = ols(target[train], x[train])
        predicted = fit.params[0] + fit.params[1] * x[~train]
        return r2_oos(target[~train], predicted), fit.rsquared

    oos_10s, in_10s = forecast(d)
    oos_1s, in_1s = forecast(regression_rows(intervals_1s, symbol))
    moved = np.mean(y != 0)
    return {
        "symbol": symbol,
        "n": len(y),
        "beta": raw_fit.params[1],
        "t": raw_fit.tvalues[1],
        "r2_raw": raw_fit.rsquared,
        "r2_norm": norm_fit.rsquared,
        "r2_windows_median": float(np.median(windows)) if windows else float("nan"),
        "forecast_10s_in": in_10s,
        "forecast_10s_oos": oos_10s,
        "forecast_1s_in": in_1s,
        "forecast_1s_oos": oos_1s,
        "moved": moved,
        "depth": float(np.median(d["avg_depth"].to_numpy())),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("prefix")
    parser.add_argument("--p1", default="../p1-order-flow")
    parser.add_argument("--symbols", default="AAPL,MSFT,INTC,CSCO,AMZN")
    parser.add_argument("--out", default="results/ofi_check.md")
    args = parser.parse_args()

    ofi_contributions = load_p1(args.p1)

    # ---- part 1: the C++ values against the study's code
    check = pl.read_csv(args.prefix + ".tob_check.csv")
    start = check.filter(pl.col("kind") == "start").row(0, named=True)
    rows = check.filter(pl.col("kind") == "change")
    e_cpp = rows["e"].to_numpy()
    e_py = python_ofi(rows, start, ofi_contributions)
    ts = rows["ts"].to_numpy()
    after = ts >= 10 * 3600 * 10**9
    win_1s = bucketed_window(ts, e_py, 10**8, 10)
    win_10s = bucketed_window(ts, e_py, 10**9, 10)
    e_match = np.array_equal(e_cpp, e_py)
    w1_match = np.array_equal(rows["ofi_1s"].to_numpy()[after], win_1s[after])
    w10_match = np.array_equal(rows["ofi_10s"].to_numpy()[after], win_10s[after])
    first_bad = int(np.argmax(e_cpp != e_py)) if not e_match else -1

    # ---- part 2: regressions
    i10 = pl.read_csv(args.prefix + ".intervals_10s.csv")
    i1 = pl.read_csv(args.prefix + ".intervals_1s.csv")
    results = [stock_results(i10, i1, s) for s in args.symbols.split(",")]

    lines = [
        "# Stage 5: order-flow imbalance inside the system",
        "",
        "Written by `analysis/ofi_check.py` from the files `signals_export` writes. "
        f"Python {platform.python_version()}, numpy {np.__version__}, polars {pl.__version__}.",
        "",
        "## The C++ OFI against the order-flow study's Python code",
        "",
        f"One stock's top-of-book changes from the reference book: AAPL, {len(rows):,} changes between "
        "09:59:50 and 11:00:00. The C++ values come from the `Signals` module the strategy uses; the Python "
        "values from `ofi_contributions` in `p1-order-flow/src/orderflow/ingest.py`, imported unchanged.",
        "",
        "| Check | Result |",
        "|---|---|",
        f"| e_n for every change: C++ equals Python exactly | {'yes' if e_match else 'NO, first at row ' + str(first_bad)} |",
        f"| OFI over the 1-second window (10 buckets of 100 ms), every change from 10:00 | {'yes' if w1_match else 'NO'} |",
        f"| OFI over the 10-second window (10 buckets of 1 s), every change from 10:00 | {'yes' if w10_match else 'NO'} |",
        f"| Sum of e_n over the hour | C++ {int(e_cpp.sum()):,}, Python {int(e_py.sum()):,} |",
        f"| Changes with an empty side (e_n = 0 by rule) | {int(((rows['bid_qty'] == 0) | (rows['ask_qty'] == 0)).sum())} |",
        "",
        "## Does OFI explain equity price moves too?",
        "",
        "Each stock's market hours from 9:45 to 15:45 (away from the opening and closing crosses), cut into "
        "10-second intervals. In each: the change of the mid-price in ticks, the summed OFI in shares, and "
        "the average depth at the best prices, (bid size + ask size) / 2. As in the study and in Cont, "
        "Kukanov & Stoikov (2014), `normalised` OFI is OFI / depth. t-statistics use Newey-West (HAC, 6 "
        "lags) standard errors.",
        "",
        "| Stock | Intervals | Mid moved in | Median depth (shares) | β (ticks per 1,000 shares) | t | R² raw OFI | R² normalised | Median R² of 30-min windows |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for r in results:
        lines.append(
            f"| {r['symbol']} | {r['n']:,} | {100 * r['moved']:.0f}% | {r['depth']:,.0f} | {1000 * r['beta']:.3f} | "
            f"{r['t']:.1f} | {r['r2_raw']:.2f} | {r['r2_norm']:.2f} | {r['r2_windows_median']:.2f} |")
    lines += [
        "",
        "For comparison: Cont, Kukanov & Stoikov report an average R² of about 65% for 10-second intervals "
        "within half-hour windows on 50 US stocks (2010); the order-flow study found 53-59% for Binance BTC "
        "and ETH perpetual futures (out of sample, 10 s).",
        "",
        "### Forecasting the next interval",
        "",
        "The next interval's mid change regressed on this interval's normalised OFI: fitted on 9:45-12:15, "
        "tested on 12:15-15:45. Out-of-sample R² is against predicting no change (negative = worse than that).",
        "",
        "| Stock | 10 s in sample | 10 s out of sample | 1 s in sample | 1 s out of sample |",
        "|---|---:|---:|---:|---:|",
    ]
    for r in results:
        lines.append(
            f"| {r['symbol']} | {100 * r['forecast_10s_in']:.2f}% | {100 * r['forecast_10s_oos']:.2f}% | "
            f"{100 * r['forecast_1s_in']:.2f}% | {100 * r['forecast_1s_oos']:.2f}% |")
    lines.append("")
    Path(args.out).write_text("\n".join(lines) + "\n")
    print(f"e match {e_match}, windows {w1_match} {w10_match}; wrote {args.out}")
    return 0 if (e_match and w1_match and w10_match) else 1


if __name__ == "__main__":
    sys.exit(main())
