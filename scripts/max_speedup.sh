#!/usr/bin/env bash
# The combined patch from scripts/build_max_patch.py, with the best compiler flag we
# measured, against the plain -O2 library. Measures the flag alone, the patch alone at
# -O2, the patch on top of the flag, and the two together, all on one node.
#
# The patch was chosen after reading the Phase 1 results, so report these numbers
# apart from that round's own logs.
#
# Usage, from the repository root, after a full build:
#   [TRIALS=3] [PATCH=results/max-speedup/max.patch] [ALLOW_DIRTY_NULL=1] \
#     scripts/max_speedup.sh <output-dir> <work-root>
# ALLOW_DIRTY_NULL=1 is for rehearsals only.
set -u

OUT=${1:?output directory}
WORK_ROOT=${2:?work root}
TRIALS=${TRIALS:-3}
PATCH=${PATCH:-results/max-speedup/max.patch}
ALLOW_DIRTY_NULL=${ALLOW_DIRTY_NULL:-0}
REPEAT=10
REPETITIONS=9
H=./build/CanMehHarness

die() { echo "max_speedup: $*" >&2; exit 1; }
stage() { echo "[$(date -u +%FT%TZ)] $*"; }

for tool in jq python3 "$H" ./build/CanMehSearch; do
  command -v "$tool" > /dev/null || die "missing: $tool"
done
[ -s "$PATCH" ] || die "missing patch: $PATCH"
[ -e "$OUT/verdicts.jsonl" ] && die "$OUT already holds results; choose another directory"

mkdir -p "$OUT/trials" "$WORK_ROOT"
cp "$PATCH" "$OUT/max.patch"
{
  echo "host: $(hostname)"
  echo "slurm job: ${SLURM_JOB_ID:-none}"
  lscpu | grep 'Model name'
  echo "logical cpus: $(nproc)"
  c++ --version | head -1
  echo "commit: $(git rev-parse HEAD)$(git diff --quiet HEAD -- src || echo ' (src modified)')"
  echo "patch sha256: $(sha256sum "$PATCH" | cut -d' ' -f1)"
  echo "repeat: $REPEAT, repetitions: $REPETITIONS, trials: $TRIALS"
  echo "started: $(date -u +%FT%TZ)"
} > "$OUT/environment.txt"

# One attempt, because keeping the first clean attempt selects for a favourable null.
init_setting() {  # label [harness flags...]
  local label=$1
  shift
  stage "init $label"
  "$H" "$@" init --work "$WORK_ROOT/$label" --repeat "$REPEAT" > "$OUT/init-$label.json"
  cp "$WORK_ROOT/$label/scratch/bench.err" "$OUT/init-$label-bench.err" 2> /dev/null
  [ "$(jq -r .ok "$OUT/init-$label.json")" = true ] || die "$label: init failed"
  [ "$(jq -r .repetitions "$WORK_ROOT/$label/meta.json")" = "$REPETITIONS" ] \
    || die "$label: wrong repetitions"
  if [ "$(jq -r .null_ok "$OUT/init-$label.json")" = true ]; then
    echo "$label null run: clean" >> "$OUT/environment.txt"
  else
    echo "$label null run: DIRTY" >> "$OUT/environment.txt"
    [ "$ALLOW_DIRTY_NULL" = 1 ] || die "$label: the null run is dirty"
  fi
}

judge() {  # measurement-file
  ./build/CanMehSearch --judge "$1" \
    || echo '{"measured":false,"faster":false,"significant":false,"unstable":false,"ratio":null,"why":"the judge could not read this measurement"}'
}

record() {  # kind name trial measurement-file
  jq -c --arg kind "$1" --arg name "$2" --argjson trial "$3" \
    --arg time "$(date -u +%FT%TZ)" --arg load "$(cut -d' ' -f1 /proc/loadavg)" \
    '. + {kind: $kind, name: $name, trial: $trial, time: $time, load: ($load | tonumber)}' \
    <<< "$(judge "$4")" >> "$OUT/verdicts.jsonl"
}

measure_patch() {  # name work
  local trial file
  stage "$1"
  for trial in $(seq 1 "$TRIALS"); do
    file=$OUT/trials/$1-$trial.json
    "$H" measure-patch --work "$2" --patch "$PATCH" --no-decision > "$file"
    [ "$(jq -r .ok "$file")" = true ] || die "$1: the patch was refused; see $file"
    record patch "$1" "$trial" "$file"
  done
}

measure_against_O2() {  # name candidate-library
  local pair=$WORK_ROOT/pair-$1 trial file
  stage "$1"
  mkdir -p "$pair"
  cp "$WORK_ROOT/O2/meta.json" "$pair/meta.json"
  ln -sf "$WORK_ROOT/O2/libfit_ref.so" "$pair/libfit_ref.so"
  ln -sf "$2" "$pair/libfit_cand.so"
  for trial in $(seq 1 "$TRIALS"); do
    file=$OUT/trials/$1-$trial.json
    "$H" measure-pair --work "$pair" --no-decision > "$file"
    record pair "$1" "$trial" "$file"
  done
}

: > "$OUT/verdicts.jsonl"
init_setting O2
init_setting O3-native --cxx "$PWD/scripts/cxx-native" --opt -O3

measure_against_O2 flag-only "$WORK_ROOT/O3-native/libfit_ref.so"
measure_patch patch-at-O2 "$WORK_ROOT/O2"
measure_patch patch-on-flag "$WORK_ROOT/O3-native"
# measure-patch leaves the patched library behind as libfit_cand.so.
cp -L "$WORK_ROOT/O3-native/libfit_cand.so" "$WORK_ROOT/max-O3-native.so"
measure_against_O2 MAX "$WORK_ROOT/max-O3-native.so"

echo "finished: $(date -u +%FT%TZ)" >> "$OUT/environment.txt"
python3 scripts/analyze_controls.py "$OUT" | tee "$OUT/summary.txt" || die "analysis failed"
stage "done"
