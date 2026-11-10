#!/usr/bin/env bash
# The pilot: search and best-of-N at a reduced budget, then the compiler
# baselines. Everything is smaller than README.md asks; the report says how.
#
# Each run gets its own work directory, because two measurements sharing one
# overwrite each other's scratch and object trees.
#
# Usage, from the repository root: scripts/pilot.sh [output-directory]
set -u

OUT=${1:-results/pilot}
GENERATIONS=10
SEEDS="1 2 3"
MODEL=claude-haiku-5-5
REGION_FILES=fit_decode.cpp,fit_mesg.cpp,fit_field_base.cpp
KEY=$HOME/.config/canmeh/api-key
H=./build/CanMehHarness
ATTEMPTS=3

mkdir -p "$OUT"
{
  echo "host: $(hostname)"
  lscpu | grep 'Model name'
  c++ --version | head -1
  echo "model: $MODEL, generations: $GENERATIONS, seeds: $SEEDS, regions: $REGION_FILES"
  echo "started: $(date -u +%FT%TZ)"
} > "$OUT/environment.txt"

# init ends with a null run, which a loaded machine can fail. Every attempt is
# kept, so the report can say how many it took.
init_until_clean() {  # work-directory run-directory [harness options...]
  local work=$1 dir=$2 attempt
  shift 2
  for attempt in $(seq 1 "$ATTEMPTS"); do
    "$H" "$@" init --work "$work" --repeat 10 > "$dir/init-$attempt.json"
    if [ "$(jq -r .null_ok "$dir/init-$attempt.json")" = true ]; then
      cp "$dir/init-$attempt.json" "$dir/init.json"
      return 0
    fi
  done
  return 1
}

run() {  # arm seed [search options...]
  local arm=$1 seed=$2 dir work
  shift 2
  dir=$OUT/$arm-$seed
  work=$PWD/build/pilot-$arm-$seed
  mkdir -p "$dir"
  if ! init_until_clean "$work" "$dir"; then
    echo "null run dirty after $ATTEMPTS attempts" > "$dir/abandoned.txt"
    return
  fi
  ./build/CanMehSearch --work "$work" --out "$dir" --generations "$GENERATIONS" \
    --seed "$seed" --region-files "$REGION_FILES" \
    --llm-api anthropic --llm-model "$MODEL" --llm-api-key-file "$KEY" "$@" \
    > "$dir/console.log" 2>&1
  echo "exit status: $?" >> "$dir/console.log"
}

for seed in $SEEDS; do
  run search "$seed" &
  run best-of-n "$seed" --no-search &
done
wait

# C1: time the -O2 reference against each other setting inside one process, by
# giving measure-pair a directory whose two library names point at the two builds.
c1() {  # label optimization-flags
  local label=$1 flags=$2 dir work pair trial
  dir=$OUT/c1-$label
  work=$PWD/build/pilot-c1-$label
  pair=$PWD/build/pilot-c1-pair-$label
  mkdir -p "$dir" "$pair"
  if ! init_until_clean "$work" "$dir" --opt "$flags"; then
    echo "null run dirty after $ATTEMPTS attempts" > "$dir/abandoned.txt"
    return
  fi
  cp build/work/meta.json "$pair/meta.json"
  ln -sf "$PWD/build/work/libfit_ref.so" "$pair/libfit_ref.so"
  ln -sf "$work/libfit_ref.so" "$pair/libfit_cand.so"
  for trial in 1 2 3; do
    "$H" measure-pair --work "$pair" --no-decision > "$dir/trial-$trial.json"
    ./build/CanMehSearch --judge "$dir/trial-$trial.json" > "$dir/verdict-$trial.json"
  done
}

c1 O3 "-O3"
c1 O3-native "-O3 -march=native"

echo "finished: $(date -u +%FT%TZ)" >> "$OUT/environment.txt"
