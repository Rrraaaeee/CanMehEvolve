#!/usr/bin/env bash
# Phase 0, from a built checkout to the summary and figure: the null run, the
# check of the calibration pairs, the trials of the decision rule, the inert
# patch, the analysis.
#
# Each trial judges the way the search does (src/Search.cpp, the loop in main):
# a candidate that is significant or unstable is measured a second time, and it
# is accepted only when candidateIsFaster passes on both. candidateIsFaster runs
# through `CanMehSearch --judge`, so it is the search's own code; only that "both"
# is restated here, in measure_twice.
#
# Trials run one at a time, so they do not disturb each other. Pairs are
# interleaved round by round, so slow drift in the machine's load falls on every
# pair rather than on whichever ran last.
#
# Usage, from the repository root:
#   [ROUNDS=30] [ALLOW_DIRTY_NULL=1] scripts/phase0.sh [output-dir] [work-dir]
# ROUNDS gives ROUNDS null trials and ROUNDS/3 of each other pair. A dirty null run
# stops the script unless ALLOW_DIRTY_NULL=1, which is for rehearsals only.
set -u

OUT=${1:-results/phase0-new}
WORK=${2:-$PWD/build/phase0-work}
REPEAT=10
REPETITIONS=9
ROUNDS=${ROUNDS:-30}
ALLOW_DIRTY_NULL=${ALLOW_DIRTY_NULL:-0}
ROOT=build/calibration
H=./build/CanMehHarness
OTHER_PAIRS="faster-1-percent faster-2-percent faster-5-percent faster-10-percent slower-10-percent"

die() { echo "phase0: $*" >&2; exit 1; }
stage() { echo "[$(date -u +%FT%TZ)] $*"; }

for tool in jq python3 "$H" ./build/CanMehSearch ./build/CanMehCalibrate; do
  command -v "$tool" > /dev/null || die "missing: $tool"
done
[ -f "$ROOT/null/meta.json" ] || die "no calibration pairs under $ROOT; build everything first"
[ -e "$OUT/verdicts.jsonl" ] && die "$OUT already holds results; choose another directory"

mkdir -p "$OUT/trials" "$OUT/inert"
{
  echo "host: $(hostname)"
  echo "slurm job: ${SLURM_JOB_ID:-none}"
  lscpu | grep 'Model name'
  echo "logical cpus: $(nproc)"
  c++ --version | head -1
  echo "commit: $(git rev-parse HEAD)$(git diff --quiet HEAD -- src || echo ' (src modified)')"
  echo "src/Search.cpp sha256: $(sha256sum src/Search.cpp | cut -d' ' -f1)"
  echo "repeat: $REPEAT, repetitions: $REPETITIONS, rounds: $ROUNDS"
  echo "started: $(date -u +%FT%TZ)"
} > "$OUT/environment.txt"

# The null run and the data's identifying numbers. One attempt, because keeping
# the first clean attempt would select for a favourable null.
stage "init"
"$H" init --work "$WORK" --repeat "$REPEAT" > "$OUT/init.json"
cp "$WORK/scratch/bench.err" "$OUT/init-bench.err" 2> /dev/null
grep 'the reference library read' "$OUT/init-bench.err" >> "$OUT/environment.txt" \
  || echo "data identity: not found in bench.err" >> "$OUT/environment.txt"
[ "$(jq -r .ok "$OUT/init.json")" = true ] || die "init failed; see $OUT/init.json"
[ "$(jq -r .repetitions "$WORK/meta.json")" = "$REPETITIONS" ] \
  || die "init recorded a different --repetitions from the $REPETITIONS used here"
if [ "$(jq -r .null_ok "$OUT/init.json")" != true ]; then
  echo "null run: DIRTY" >> "$OUT/environment.txt"
  [ "$ALLOW_DIRTY_NULL" = 1 ] || die "the null run is dirty; see $OUT/init.json"
  stage "continuing past a dirty null run, because ALLOW_DIRTY_NULL=1"
else
  echo "null run: clean" >> "$OUT/environment.txt"
fi

stage "CanMehCalibrate"
./build/CanMehCalibrate --repeat "$REPEAT" --repetitions "$REPETITIONS" > "$OUT/calibrate.txt" 2>&1
status=$?
echo "CanMehCalibrate exit status: $status" >> "$OUT/calibrate.txt"
[ "$status" -eq 0 ] || die "CanMehCalibrate failed; see $OUT/calibrate.txt"
cp "$ROOT/calibration.json" "$OUT/calibration.json"

min_time() {
  local value
  value=$(jq -r --arg p "$1" '[.. | objects | select(.pair == $p) | .min_time] | first' \
    "$OUT/calibration.json")
  [ -n "$value" ] && [ "$value" != null ] || die "no min_time recorded for $1"
  echo "$value"
}

judge() {  # measurement-file
  ./build/CanMehSearch --judge "$1" \
    || echo '{"measured":false,"faster":false,"significant":false,"unstable":false,"ratio":null,"why":"the judge could not read this measurement"}'
}

measure_twice() {  # stem command...
  local stem=$1 first second=null
  shift
  "$@" > "$stem.json"
  first=$(judge "$stem.json")
  if [ "$(jq -r '.significant or .unstable' <<< "$first")" = true ]; then
    "$@" > "$stem-second.json"
    second=$(judge "$stem-second.json")
  fi
  jq -cn --argjson first "$first" --argjson second "$second" '
    $first + {second: $second,
              accepted: ($first.faster and ($second.faster // false)),
              kept_ratio: (if ($second.measured // false) and $second.ratio > $first.ratio
                           then $second.ratio else $first.ratio end)}'
}

trial() {  # pair round
  local pair=$1 round=$2 time verdict
  time=$(min_time "$pair") || exit 1
  verdict=$(measure_twice "$OUT/trials/$pair-$(printf '%02d' "$round")" \
    "$H" measure-pair --work "$ROOT/$pair" --repeat "$REPEAT" --repetitions "$REPETITIONS" \
    --min-time "$time" --no-decision)
  jq -c --arg pair "$pair" --argjson round "$round" --arg time "$(date -u +%FT%TZ)" \
    --arg load "$(cut -d' ' -f1 /proc/loadavg)" \
    '. + {pair: $pair, round: $round, time: $time, load: ($load | tonumber)}' \
    <<< "$verdict" >> "$OUT/verdicts.jsonl"
}

stage "trials: $ROUNDS rounds"
: > "$OUT/verdicts.jsonl"
for round in $(seq 1 "$ROUNDS"); do
  trial null "$round"
  if (( round % 3 == 0 )); then
    n=$(( round / 3 ))
    for pair in $OTHER_PAIRS; do
      trial "$pair" "$n"
    done
  fi
done

# A comment changes no instruction, so this patch's true ratio is 1.000.
stage "inert patch"
region=$("$H" regions --work "$WORK" --file fit_decode.cpp \
  | jq -r '.regions[] | select(.label == "Decode::Decode" and .ordinal == 0) | .id')
[ -n "$region" ] || die "could not find Decode::Decode in the region list"
"$H" show --work "$WORK" --region "$region" > "$OUT/inert/show.json"
{
  echo "// inert: a comment changes no instruction"
  jq -r .text "$OUT/inert/show.json"
} > "$OUT/inert/replacement.cpp"
"$H" splice --work "$WORK" --region "$region" --replacement "$OUT/inert/replacement.cpp" \
  --out "$OUT/inert/inert.patch" > "$OUT/inert/splice.json"
[ "$(jq -r .ok "$OUT/inert/splice.json")" = true ] || die "splice failed; see $OUT/inert/splice.json"
measure_twice "$OUT/inert/measure" "$H" measure-patch --work "$WORK" \
  --patch "$OUT/inert/inert.patch" --no-decision > "$OUT/inert/verdict.json"

stage "analysis"
echo "finished: $(date -u +%FT%TZ)" >> "$OUT/environment.txt"
python3 scripts/analyze_phase0.py "$OUT" | tee "$OUT/summary.txt" || die "analysis failed"
python3 scripts/plot_detection.py "$OUT" "$OUT/detection.pdf" || die "figure failed"

expected=$(( ROUNDS + 5 * ROUNDS / 3 ))
actual=$(jq -s 'map(select(.measured)) | length' "$OUT/verdicts.jsonl")
stage "done: $actual of $expected trials measured; inert: $(jq -c '{accepted, ratio, kept_ratio, p_value, unstable}' "$OUT/inert/verdict.json")"
[ "$actual" -eq "$expected" ] || die "some trials were not measured; see $OUT/verdicts.jsonl"
