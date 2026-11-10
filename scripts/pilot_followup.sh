#!/usr/bin/env bash
# Follow-ups to scripts/pilot.sh, decided after reading its results.
#
# C1: pilot.sh abandoned both compiler baselines. -O3 failed its own null check
# on a loaded machine, though the libraries were built, and `--opt "-O3
# -march=native"` cannot compile because CanMehHarness passes it as one flag. So
# -O3 reuses the built library, and -march=native goes through a compiler wrapper.
#
# Re-measurement: the lowest ratio of the pilot, best-of-n-1 generation 2, which
# the rule rejected because its processes disagreed. Selected after the fact, so
# these measurements are reported apart from the runs' own logs.
#
# Usage, from the repository root: scripts/pilot_followup.sh [output-directory]
set -u

OUT=${1:-results/pilot/followup}
H=./build/CanMehHarness
S=./build/CanMehSearch
TRIALS=3
mkdir -p "$OUT"
echo "started: $(date -u +%FT%TZ)" > "$OUT/environment.txt"

c1_pair() {  # label work-directory-holding-the-other-build
  local label=$1 work=$2 pair trial
  pair=$PWD/build/pilot-c1-pair-$label
  mkdir -p "$OUT/c1-$label" "$pair"
  cp build/work/meta.json "$pair/meta.json"
  ln -sf "$PWD/build/work/libfit_ref.so" "$pair/libfit_ref.so"
  ln -sf "$work/libfit_ref.so" "$pair/libfit_cand.so"
  for trial in $(seq 1 "$TRIALS"); do
    "$H" measure-pair --work "$pair" --no-decision > "$OUT/c1-$label/trial-$trial.json"
    "$S" --judge "$OUT/c1-$label/trial-$trial.json" > "$OUT/c1-$label/verdict-$trial.json"
  done
}

c1_pair O3 "$PWD/build/pilot-c1-O3"

"$H" --cxx "$PWD/scripts/cxx-native" --opt -O3 init --work "$PWD/build/pilot-c1-O3-native" \
  --repeat 10 > "$OUT/init-O3-native.json"
c1_pair O3-native "$PWD/build/pilot-c1-O3-native"

mkdir -p "$OUT/remeasure"
for trial in $(seq 1 "$TRIALS"); do
  "$H" measure-patch --work build/work --patch results/pilot/best-of-n-1/patches/gen_2.patch \
    --no-decision > "$OUT/remeasure/trial-$trial.json"
  "$S" --judge "$OUT/remeasure/trial-$trial.json" > "$OUT/remeasure/verdict-$trial.json"
done

cut -d' ' -f1-3 /proc/loadavg > "$OUT/load-at-end.txt"
echo "finished: $(date -u +%FT%TZ)" >> "$OUT/environment.txt"
