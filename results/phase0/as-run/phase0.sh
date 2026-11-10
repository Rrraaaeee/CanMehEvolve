#!/usr/bin/env bash
# Phase 0: calibrate the decision rule in src/Search.cpp against the calibration
# pairs, whose true ratios are known by construction.
#
# Trials run one at a time, so they do not disturb each other. Pairs are
# interleaved round by round, so slow drift in the machine's load falls on every
# pair rather than on whichever ran last. The verdicts come from
# `CanMehSearch --judge`, which is the same code the search runs.
#
# Usage, from the repository root: scripts/phase0.sh [output-directory]
set -u

OUT=${1:-results/phase0}
REPEAT=10
REPETITIONS=9
ROUNDS=30
ROOT=build/calibration
H=./build/CanMehHarness

mkdir -p "$OUT/trials"
{
  echo "host: $(hostname)"
  lscpu | grep 'Model name'
  c++ --version | head -1
  echo "repeat: $REPEAT, repetitions: $REPETITIONS, rounds: $ROUNDS"
  echo "started: $(date -u +%FT%TZ)"
} > "$OUT/environment.txt"

./build/CanMehCalibrate --repeat "$REPEAT" --repetitions "$REPETITIONS" \
  > "$OUT/calibrate.txt" 2>&1
echo "CanMehCalibrate exit status: $?" >> "$OUT/calibrate.txt"
cp "$ROOT/calibration.json" "$OUT/calibration.json"

min_time() {
  jq -r --arg p "$1" '[.. | objects | select(.pair == $p) | .min_time] | first' \
    "$OUT/calibration.json"
}

trial() {  # pair round
  local pair=$1 round=$2 file
  file="$OUT/trials/$pair-$(printf '%02d' "$round").json"
  "$H" measure-pair --work "$ROOT/$pair" --repeat "$REPEAT" --repetitions "$REPETITIONS" \
    --min-time "$(min_time "$pair")" --no-decision > "$file"
  ./build/CanMehSearch --judge "$file" \
    | jq -c --arg pair "$pair" --argjson round "$round" \
         --arg time "$(date -u +%FT%TZ)" --arg load "$(cut -d' ' -f1 /proc/loadavg)" \
         '. + {pair: $pair, round: $round, time: $time, load: ($load | tonumber)}' \
    >> "$OUT/verdicts.jsonl"
}

: > "$OUT/verdicts.jsonl"
for round in $(seq 1 "$ROUNDS"); do
  trial null "$round"
  if (( round % 3 == 0 )); then
    n=$(( round / 3 ))
    for pair in faster-1-percent faster-2-percent faster-5-percent faster-10-percent \
                slower-10-percent; do
      trial "$pair" "$n"
    done
  fi
done

echo "finished: $(date -u +%FT%TZ)" >> "$OUT/environment.txt"
