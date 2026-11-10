#!/usr/bin/env bash
# Control C1 and the patch measurements, all on one node so they can be compared.
#
# C1 builds the reference library at -O3 and at -O3 -march=native, then times each
# against the -O2 library in one process, so both see the same clock. The patch
# measurements time chosen patches against the -O2 library: each half of the
# two-function core alone, the two together, and the best search patch.
#
# These patches were chosen after reading the Phase 1 results, so report these
# numbers apart from that round's own logs.
#
# Usage, from the repository root, after a full build:
#   [TRIALS=3] [SKIP_C1=1] [ALLOW_DIRTY_NULL=1] \
#     scripts/controls.sh <output-dir> <work-root> [name=patch ...]
# Each name=patch adds a patch, for example c2=results/c2/human.patch. SKIP_C1=1
# measures the patches only. ALLOW_DIRTY_NULL=1 is for rehearsals only.
set -u

OUT=${1:?output directory}
WORK_ROOT=${2:?work root}
shift 2
TRIALS=${TRIALS:-3}
SKIP_C1=${SKIP_C1:-0}
ALLOW_DIRTY_NULL=${ALLOW_DIRTY_NULL:-0}
REPEAT=10
REPETITIONS=9
H=./build/CanMehHarness
GETMESG=results/phase1-opus/best-of-n-4/best.patch
COPYCTOR=results/phase1-opus-smoke/best-of-n-1/best.patch
SEARCH_BEST=results/phase1-opus/search-3/best.patch
FAILED=0

die() { echo "controls: $*" >&2; exit 1; }
stage() { echo "[$(date -u +%FT%TZ)] $*"; }

for tool in jq python3 "$H" ./build/CanMehSearch; do
  command -v "$tool" > /dev/null || die "missing: $tool"
done
for patch in "$GETMESG" "$COPYCTOR" "$SEARCH_BEST"; do
  [ -s "$patch" ] || die "missing patch: $patch"
done
for extra in "$@"; do
  [ -s "${extra#*=}" ] || die "missing patch: ${extra#*=}"
done
[ -e "$OUT/verdicts.jsonl" ] && die "$OUT already holds results; choose another directory"

mkdir -p "$OUT/trials" "$OUT/patches" "$WORK_ROOT"
{
  echo "host: $(hostname)"
  echo "slurm job: ${SLURM_JOB_ID:-none}"
  lscpu | grep 'Model name'
  echo "logical cpus: $(nproc)"
  c++ --version | head -1
  echo "commit: $(git rev-parse HEAD)$(git diff --quiet HEAD -- src || echo ' (src modified)')"
  echo "repeat: $REPEAT, repetitions: $REPETITIONS, trials: $TRIALS"
  echo "started: $(date -u +%FT%TZ)"
} > "$OUT/environment.txt"

# One attempt, because keeping the first clean attempt selects for a favourable null.
# Returns non-zero when this setting's numbers cannot be used.
init_setting() {  # label [harness flags...]
  local label=$1
  shift
  stage "init $label"
  "$H" "$@" init --work "$WORK_ROOT/$label" --repeat "$REPEAT" > "$OUT/init-$label.json"
  cp "$WORK_ROOT/$label/scratch/bench.err" "$OUT/init-$label-bench.err" 2> /dev/null
  [ "$(jq -r .ok "$OUT/init-$label.json")" = true ] || {
    echo "$label: init FAILED" >> "$OUT/environment.txt"
    return 1
  }
  [ "$(jq -r .repetitions "$WORK_ROOT/$label/meta.json")" = "$REPETITIONS" ] || {
    echo "$label: wrong repetitions" >> "$OUT/environment.txt"
    return 1
  }
  if [ "$(jq -r .null_ok "$OUT/init-$label.json")" = true ]; then
    echo "$label null run: clean" >> "$OUT/environment.txt"
    return 0
  fi
  echo "$label null run: DIRTY" >> "$OUT/environment.txt"
  [ "$ALLOW_DIRTY_NULL" = 1 ]
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

measure_patch() {  # name patch
  local trial file
  stage "patch $1"
  for trial in $(seq 1 "$TRIALS"); do
    file=$OUT/trials/patch-$1-$trial.json
    "$H" measure-patch --work "$WORK_ROOT/O2" --patch "$2" --no-decision > "$file"
    record patch "$1" "$trial" "$file"
  done
}

measure_setting() {  # label
  local label=$1 pair=$WORK_ROOT/pair-$1 trial file
  stage "C1 $label against -O2"
  mkdir -p "$pair"
  cp "$WORK_ROOT/O2/meta.json" "$pair/meta.json"
  ln -sf "$WORK_ROOT/O2/libfit_ref.so" "$pair/libfit_ref.so"
  ln -sf "$WORK_ROOT/$label/libfit_ref.so" "$pair/libfit_cand.so"
  for trial in $(seq 1 "$TRIALS"); do
    file=$OUT/trials/c1-$label-$trial.json
    "$H" measure-pair --work "$pair" --no-decision > "$file"
    record c1 "$label" "$trial" "$file"
  done
}

: > "$OUT/verdicts.jsonl"
init_setting O2 || die "the -O2 work directory cannot be used; see $OUT/environment.txt"

if [ "$SKIP_C1" != 1 ]; then
  if init_setting O3 --opt -O3; then measure_setting O3; else FAILED=1; fi
  # CanMehHarness passes --opt as one flag, so -march=native goes through a wrapper.
  if init_setting O3-native --cxx "$PWD/scripts/cxx-native" --opt -O3; then
    measure_setting O3-native
  else
    FAILED=1
  fi
fi

# The two patches touch different files, so their diffs concatenate cleanly.
cat "$GETMESG" > "$OUT/patches/getmesg.patch"
cat "$COPYCTOR" > "$OUT/patches/copyctor.patch"
cat "$GETMESG" "$COPYCTOR" > "$OUT/patches/both.patch"

measure_patch getmesg "$OUT/patches/getmesg.patch"
measure_patch copyctor "$OUT/patches/copyctor.patch"
measure_patch both "$OUT/patches/both.patch"
measure_patch search-3 "$SEARCH_BEST"
for extra in "$@"; do
  measure_patch "${extra%%=*}" "${extra#*=}"
done

echo "finished: $(date -u +%FT%TZ)" >> "$OUT/environment.txt"
python3 scripts/analyze_controls.py "$OUT" | tee "$OUT/summary.txt" || die "analysis failed"
[ "$FAILED" -eq 0 ] || die "a compiler setting could not be measured; see $OUT/environment.txt"
stage "done"
