#!/usr/bin/env bash
# One Phase 1 run from a built checkout: the null run, then one arm and seed of the
# search. Both arms take the same model, budget and region draw; best-of-n adds
# --no-search, which is control C3.
#
# Usage, from the repository root:
#   MODEL=<id> [GENERATIONS=40] [SAMPLE_SECONDS=20] [LLM_API=anthropic] \
#     [KEY=<key-file>] [ALLOW_DIRTY_NULL=1] [ALLOW_NO_PROFILE=1] \
#     scripts/phase1.sh search|best-of-n <seed> <output-dir> <work-dir>
# MODEL has no default, so a full run cannot fall back to a test model by accident.
# The two ALLOW_ settings are for rehearsals only.
set -u

ARM=${1:?arm: search or best-of-n}
SEED=${2:?seed}
OUT=${3:?output directory}
WORK=${4:?work directory}
MODEL=${MODEL:?set MODEL to the model id}
GENERATIONS=${GENERATIONS:-40}
SAMPLE_SECONDS=${SAMPLE_SECONDS:-20}
LLM_API=${LLM_API:-anthropic}
KEY=${KEY:-$HOME/.config/canmeh/api-key}
ALLOW_DIRTY_NULL=${ALLOW_DIRTY_NULL:-0}
ALLOW_NO_PROFILE=${ALLOW_NO_PROFILE:-0}
REPEAT=10
REPETITIONS=9
H=./build/CanMehHarness

die() { echo "phase1: $*" >&2; exit 1; }
stage() { echo "[$(date -u +%FT%TZ)] $ARM-$SEED: $*"; }

case $ARM in
  search) ARM_FLAGS=() ;;
  best-of-n) ARM_FLAGS=(--no-search) ;;
  *) die "unknown arm: $ARM" ;;
esac

for tool in jq python3 "$H" ./build/CanMehSearch; do
  command -v "$tool" > /dev/null || die "missing: $tool"
done
[ -r "$KEY" ] || die "cannot read the key file $KEY"
[ -e "$OUT/results.jsonl" ] && die "$OUT already holds results; choose another directory"

mkdir -p "$OUT"
{
  echo "host: $(hostname)"
  echo "slurm job: ${SLURM_JOB_ID:-none}"
  lscpu | grep 'Model name'
  echo "logical cpus: $(nproc)"
  c++ --version | head -1
  echo "commit: $(git rev-parse HEAD)$(git diff --quiet HEAD -- src || echo ' (src modified)')"
  echo "src/Search.cpp sha256: $(sha256sum src/Search.cpp | cut -d' ' -f1)"
  echo "arm: $ARM, seed: $SEED, model requested: $MODEL, api: $LLM_API"
  echo "generations: $GENERATIONS, sample seconds: $SAMPLE_SECONDS, repeat: $REPEAT"
  echo "started: $(date -u +%FT%TZ)"
} > "$OUT/environment.txt"

# Check the profiler before any budget is spent. Without it the search falls back
# to uniform draws, which the README warns against.
if [ "$SAMPLE_SECONDS" != 0 ] && [ "$ALLOW_NO_PROFILE" != 1 ]; then
  perf record -o "$OUT/perf-check.data" -- true > /dev/null 2>&1 \
    || die "perf cannot record on this node, so the search would draw regions uniformly"
  rm -f "$OUT/perf-check.data"
fi

# One attempt, because keeping the first clean attempt selects for a favourable null.
stage "init"
"$H" init --work "$WORK" --repeat "$REPEAT" > "$OUT/init.json"
cp "$WORK/scratch/bench.err" "$OUT/init-bench.err" 2> /dev/null
grep 'the reference library read' "$OUT/init-bench.err" >> "$OUT/environment.txt"
[ "$(jq -r .ok "$OUT/init.json")" = true ] || die "init failed; see $OUT/init.json"
[ "$(jq -r .repetitions "$WORK/meta.json")" = "$REPETITIONS" ] \
  || die "init recorded a different --repetitions from the $REPETITIONS calibrated"
if [ "$(jq -r .null_ok "$OUT/init.json")" != true ]; then
  echo "null run: DIRTY" >> "$OUT/environment.txt"
  [ "$ALLOW_DIRTY_NULL" = 1 ] || die "the null run is dirty; see $OUT/init.json"
  stage "continuing past a dirty null run, because ALLOW_DIRTY_NULL=1"
else
  echo "null run: clean" >> "$OUT/environment.txt"
fi

stage "search: $GENERATIONS generations"
./build/CanMehSearch --work "$WORK" --out "$OUT" --generations "$GENERATIONS" --seed "$SEED" \
  --sample-seconds "$SAMPLE_SECONDS" --llm-api "$LLM_API" --llm-model "$MODEL" \
  --llm-api-key-file "$KEY" ${ARM_FLAGS[@]+"${ARM_FLAGS[@]}"} > "$OUT/console.log" 2>&1
status=$?
echo "exit status: $status" >> "$OUT/console.log"
echo "finished: $(date -u +%FT%TZ)" >> "$OUT/environment.txt"
if grep -q 'no usable profile' "$OUT/console.log"; then
  echo "profile: NONE, regions drawn uniformly" >> "$OUT/environment.txt"
  stage "warning: no usable profile, so regions were drawn uniformly"
else
  echo "profile: used" >> "$OUT/environment.txt"
fi
[ "$status" -eq 0 ] || die "CanMehSearch exited with status $status; see $OUT/console.log"
stage "done: $(tail -n 1 "$OUT/credits.csv")"
