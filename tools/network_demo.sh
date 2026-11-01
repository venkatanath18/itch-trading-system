#!/bin/bash
# Stretch: replay the start of the day as MoldUDP64 over UDP on localhost,
# with packets dropped on purpose, and check the receiver recovers every message.
#
#   tools/network_demo.sh [DAY] [MESSAGES] [LOSS]
set -euo pipefail
DAY=${1:-12302019}
MESSAGES=${2:-10000000}
LOSS=${3:-0.001}
cd "$(dirname "$0")/.."
ITCH=data/itch/$DAY.NASDAQ_ITCH50
DERIVED=data/derived/$DAY
BIN=build/release/src
LOG=$(mktemp)

$BIN/mold_receive "$ITCH" --sizing "$DERIVED.sizing.csv" --global "$DERIVED.global.txt" \
    --messages "$MESSAGES" --out results/network.tmp 2> "$LOG" &
receiver=$!
until grep -q "receiver ready" "$LOG"; do sleep 0.2; done
sender=$($BIN/mold_replay "$ITCH" --messages "$MESSAGES" --loss "$LOSS")
echo "$sender"
wait $receiver
cat "$LOG"
# put the sender's summary into the report
sed "s|sender line not given|${sender//|/\\|}|" results/network.tmp > results/network.md
rm -f results/network.tmp "$LOG"
