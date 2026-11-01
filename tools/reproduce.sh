#!/bin/bash
# Reproduce every number in results/ (and so in the README) from scratch:
#
#   tools/reproduce.sh [DAY]          # DAY defaults to 12302019
#
# Builds and tests all four configurations, downloads the day if it isn't
# there, then runs stages 1 to 11 and the network stretch goal in order. On an
# Apple M1 the runs take about 50 minutes once the builds are done; the
# slowest parts are the differential test and the book timings.
# Run it on a quiet machine: the latency numbers depend on it.
set -euo pipefail

DAY=${1:-12302019}
cd "$(dirname "$0")/.."
ROOT=$(pwd)
ITCH=data/itch/$DAY.NASDAQ_ITCH50
DERIVED=data/derived/$DAY
SIZING=(--sizing "$DERIVED.sizing.csv" --global "$DERIVED.global.txt")
BIN=build/release
JOBS=${JOBS:-4}
P1=${P1:-../p1-order-flow}
PIN=()
if [ "$(uname)" = "Linux" ]; then PIN=(--pin 2); fi

step() { echo; echo "=== $*"; }
mkdir -p results/raw data/derived
rm -f results/raw/*.csv

step "Stage 0: build and test Debug, Release, ASan+UBSan and TSan"
{
    echo "# Tests in every configuration"
    echo
    echo "Written by \`tools/reproduce.sh\`. Unit tests, the randomized book comparison, the system tests"
    echo "on a synthetic day, the fuzz smoke test and (outside the sanitizer builds) the no-allocation test."
    echo
    echo "| Configuration | Result |"
    echo "|---|---|"
} > results/tests.md
for preset in debug release asan tsan; do
    cmake --preset "$preset" > /dev/null
    cmake --build --preset "$preset" -j "$JOBS" > /dev/null
    summary=$(ctest --preset "$preset" 2>&1 | grep -E "tests passed" || echo "FAILED")
    echo "  $preset: $summary"
    echo "| $preset | $summary |" >> results/tests.md
done

step "Data: $ITCH"
if [ ! -f "$ITCH" ]; then python3 tools/download_itch.py "$DAY"; fi

step "Stage 1: the day"
$BIN/src/day_summary "$ITCH" --md5-file "data/raw/$DAY.NASDAQ_ITCH50.gz.md5" --out results/day_summary.md \
    --sizing "$DERIVED.sizing.csv" --global "$DERIVED.global.txt"

step "Stage 2: parser throughput and fuzzing"
$BIN/bench/parse_bench "$ITCH" --global "$DERIVED.global.txt" --out results/parser.md
FUZZ_RUNS=20000000
start=$(date +%s)
build/asan/tests/fuzz_parser --random $FUZZ_RUNS tests/fuzz/seeds > /dev/null
fuzz_seconds=$(( $(date +%s) - start ))
cat > results/fuzz.md <<EOF
# Stage 2: fuzzing the parser

Written by \`tools/reproduce.sh\`. The standalone mutation fuzzer (\`tests/fuzz/standalone_main.cpp\`)
ran $FUZZ_RUNS inputs mutated from the seeds in \`tests/fuzz/seeds\` (bit flips, random bytes,
truncation, broken length prefixes, inserted bytes, spliced seeds) through the parser, built with
AddressSanitizer and UndefinedBehaviorSanitizer, each input in a buffer of exactly its size.
No crash, no read past the end, no undefined behaviour, in $fuzz_seconds s.

CI also runs the libFuzzer build (\`-DBUILD_FUZZER=ON\`, clang on Linux) for two minutes on every push.
EOF

step "Stage 3: reference book with invariants"
$BIN/src/book_check "$ITCH" --out results/reference_book.md

step "Stage 4: fast books against the reference, then timed"
$BIN/src/book_diff "$ITCH" "${SIZING[@]}" --out results/book_diff.md
# the fast designs three times each (the report shows the median and the range)
for rep in 1 2 3; do
    for book in hash-linear hash-sorted flat-sorted flat-linear local-hash-sorted hash-tick hash-tick256; do
        $BIN/bench/book_bench "$ITCH" --book $book "${SIZING[@]}" --csv results/raw/book_bench.csv
    done
done
for book in reference legacy; do
    $BIN/bench/book_bench "$ITCH" --book $book "${SIZING[@]}" --csv results/raw/book_bench.csv
done
$BIN/bench/book_bench --report results/raw/book_bench.csv --out results/book.md

step "Stage 5: OFI inside the system, checked against the order-flow study"
$BIN/src/signals_export "$ITCH" --prefix "$DERIVED"
if [ -x "$P1/.venv/bin/python" ]; then
    "$P1/.venv/bin/python" analysis/ofi_check.py "$DERIVED" --p1 "$P1" --out results/ofi_check.md
else
    echo "  skipped: needs the order-flow study at $P1 with its .venv (numpy, polars, statsmodels)"
fi

step "Stage 6: calibration"
$BIN/src/calibrate "$ITCH" "${SIZING[@]}" --params "$DERIVED.params.csv" --out results/calibration.md

step "Stage 7: cost of the risk checks"
$BIN/bench/risk_bench --out results/risk.md

step "Stages 6-11: the strategy over the day"
TRADE=($BIN/src/trade "$ITCH" "${SIZING[@]}" --params "$DERIVED.params.csv")
run() { local label=$1; shift; "${TRADE[@]}" --label "$label" "$@"; }
run base --log results/raw/base_log.csv
run base-again
run base-threaded --threads ${PIN[@]+"${PIN[@]}"}
run no-kill --no-kill-switch
run ofi --ofi
run no-kill-ofi --no-kill-switch --ofi
for us in 10 100 1000; do
    run "latency-${us}us" --latency-us $us
    run "no-kill-latency-${us}us" --no-kill-switch --latency-us $us
done
run no-kill-gamma-x0.25 --no-kill-switch --gamma-scale 0.25
run no-kill-gamma-x4 --no-kill-switch --gamma-scale 4
$BIN/src/trade --report results/raw/trade_runs.csv --stocks results/raw/trade_stocks.csv --out results/strategy.md

step "Stage 9: no allocation on the hot path, and queue costs"
alloc_line=$($BIN/src/trade_alloc_check "$ITCH" "${SIZING[@]}" --params "$DERIVED.params.csv" --label alloc-check \
    --runs-csv results/raw/alloc_runs.csv --stocks-csv results/raw/alloc_stocks.csv 2>&1 | grep "allocations during")
cat > results/allocations.md <<EOF
# Stage 9: allocations on the hot path

Written by \`tools/reproduce.sh\`. \`trade_alloc_check\` is the \`trade\` program linked with a replacement
global \`operator new\` that counts every call (\`src/apps/alloc_counter.cpp\`). It builds the whole
system first (start-up may allocate), then counts the calls during the replay of the whole day with the
base configuration:

    $alloc_line

The test \`no_allocation_on_the_hot_path\` checks the same on a synthetic day, with the log written by
one thread and by two, in every Debug and Release build.
EOF
$BIN/bench/queue_bench --out results/queues.md

step "Stage 10: latency, three measured runs"
for i in 1 2 3; do
    run "measure-$i" --measure --latency-csv results/raw/latency.csv ${PIN[@]+"${PIN[@]}"}
done
$BIN/src/trade --latency-report results/raw/latency.csv --runs results/raw/trade_runs.csv --out results/latency.md

step "Stretch: the feed over UDP with packet loss"
tools/network_demo.sh "$DAY"

step "Done. Results in $ROOT/results"
