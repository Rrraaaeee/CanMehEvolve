# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A CS6219 assignment: an LLM-in-the-loop evolutionary search for patches that speed up the Garmin FIT SDK decoder (tag 21.201.0), evaluated with the controls published LLM-search papers omit. `README.md` is the full specification (about 2,000 lines). Its **Glossary**, **Measurement**, **Traps** and **Defaults** sections are the reference for anything not covered here.

## Hard constraints

- `src/Search.cpp` is the only file the assignment edits. It ships with five structs and seven declared-but-unwritten functions (`main`, `systemPrompt`, `userPrompt`, `splitReply`, `codeBlocks`, `candidateIsFaster`, `runHarnessCommand`). The other five programs are complete, so treat their behavior as given.
- The README's descriptions of what `CanMehSearch` does (files under `--out`, population, profiling pass, re-measurement, CLI flags) are requirements on code that does not exist yet, not inherited behavior.
- The search loop may not be written in Python. C++ is expected, so it can use the headers in `src/utilities/`.
- Never commit the FIT SDK. It is licensed; CMake fetches it into the build directory.
- Every number in the report must come from a run that really happened. Fabricating a measurement is academic misconduct. The author must be able to explain every submitted line.
- `candidateIsFaster` must compute its decision from `reply["timings"]` alone. Under `--no-decision`, `ratio`, `significant` and every other statistic are absent, so reading them throws. Presenting the harness's own verdict as the result is prohibited.

## Commands

There is no test suite and no lint configuration. `CanMehCalibrate` is the closest thing to a test: it exits non-zero when a required calibration row misses.

```sh
# Build everything, including the calibration libraries. The five-target command
# in the README omits those libraries, so measure-pair then fails at stage load.
cmake -S . -B build && cmake --build build -j

# Create a work directory (a measurement, so run it on a compute node).
# Check that the printed null_ok is true; if false, every number from it is worthless.
./build/CanMehHarness init --work build/work --repeat 10

# Measure all twelve calibration pairs.
./build/CanMehCalibrate --repeat 10

# One calibration trial for the decision rule. Pass the pair's min_time recorded in
# build/calibration/calibration.json; measure-pair otherwise falls back to 0.25s.
./build/CanMehHarness measure-pair --work build/calibration/null --no-decision

# Evaluate one candidate by hand. Every subcommand takes --work; those resolving a
# region id also take --max-lines. --help on any tool or subcommand lists every flag.
H=./build/CanMehHarness; W=build/work
$H regions --work $W --max-lines 250
$H show    --work $W --region <id> [--patch parent.patch]
mkdir -p runs/exp1/patches   # splice reports ok and writes nothing if this is missing
$H splice  --work $W --region <id> [--patch parent.patch] --replacement new.cpp \
           [--includes inc.txt] --out runs/exp1/patches/gen_N.patch
$H measure-patch --work $W --patch runs/exp1/patches/gen_N.patch --no-decision

# Run the search once written. --llm-api-key-file is required.
./build/CanMehSearch --work build/work --out runs/exp1 --generations 40 \
    --sample-seconds 20 --llm-api-key-file ~/.config/canmeh/api-key
```

`--opt` and `--cxxstd` go before the subcommand and need their own work directory per setting (control C1): `./build/CanMehHarness --opt -O3 init --work build/work-O3 --repeat 10`. Leaving `--patch` off `show` and `splice` works from the unmodified baseline, which is how the best-of-N control (C3) is built.

## Architecture

Six executables, all from `CMakeLists.txt`. Dependencies are pinned and fetched into `build/deps`; default paths are compiled in as `CANMEH_DEFAULT_*` definitions.

- `CanMehSearch` (`src/Search.cpp`) decides what to try next and never touches the build. It spawns `CanMehHarness` through `runHarnessCommand` → `utilities::runForJsonReply`.
- `CanMehHarness` (`src/Harness.cpp`) owns patching, compiling and measuring. Each subcommand prints exactly one JSON object and all state lives in the work directory, so a loop can stop and restart at any generation.
- `CanMehBench` (`src/BenchmarkProcess.cpp`) `dlopen`s two shared libraries `RTLD_LOCAL` into one process, runs correctness checks, then interleaves their timing under Google Benchmark so both see the same clock and machine conditions.
- `CanMehCalibrate` (`src/Calibrate.cpp`) runs `measure-pair` over the calibration pairs.
- `CanMehDecodeForSampler` decodes one `-g` library repeatedly while `utilities::measureLineSamples` runs `perf` (Linux) or `sample` (macOS); `utilities::weighRanges` turns that into per-region weights.
- `CanMehDataGenerator` writes the four synthetic `.fit` files. It is compiled with `-ffp-contract=off` and rounds its sine-derived values so the bytes are identical across platforms; keep both.

One generation: `regions` → `show` → prompt → `utilities::requestModelReply` → `splitReply` → `splice` → `measure-patch --no-decision` → `candidateIsFaster` → population update. A candidate that passes is measured a second time, and the second measurement must agree.

**Patches accumulate.** Each individual is a unified diff against stock SDK holding one region rewrite per ancestor, so a descendant inherits its parent's speedup. Credit a generation with `improved_on_parent`, not `significant` or `significant_at_min_effect`, which reward a whole lineage for one discovery.

**Work directory** (`--work`): `baseline/` (pristine SDK), `tree/` (patched copy), `obj/`, `libfit_ref` (linked once at `init`, so the denominator is fixed for the run), `libfit_cand`, `scratch/` (`gb/proc-NN.json`, `bench.err`) and `meta.json` (compiler and the `--repeat`, `--repetitions`, `--min-time` every later measurement inherits). Each calibration pair under `build/calibration/` is also a work directory, accepted by `measure-pair` only.

**Calibration pairs** compile `src/workloads/CalibrationLoop.cpp` twice with different work-unit counts, so the true ratio is the quotient of the two counts in `meta.json`. A ratio below 1.0 means the candidate is faster.

## Reading harness output

- Read `ok` before `timings`. A refusal names a `stage` (`patch`, `compile`, `link`, `crash`, `load`, `correctness`, `profile`, `threading`, and others listed under **Monitoring a run**). Treat an unrecognized stage as a rejection.
- `candidate_ns` and `reference_ns` are already nanoseconds per decode. Do not divide by the iteration counts, which also need not match between the two series.
- Check `ragged` before pairing repetitions across processes.
- `results.jsonl` records are not uniform: a failed model call or a reply with no usable code block has seven keys only.
- Region ids are positions in the list at one `--max-lines`. Changing it renumbers every id.
- `splice` ignores `#include` lines inside the replacement; they go in the `--includes` file. A replacement must include the function signature and braces.

## Measurement pitfalls

- `--repeat` defaults to 1, where an inert patch is called significant about two times in five. Between 2 and 7 the sign-flip test cannot reach p < 0.01. Use 10.
- The sign-flip test reads direction only, so also require an effect floor; 0.02 is the expected value.
- Use the same `--repeat` and `--repetitions` in search and calibration. Do not match `--min-time`: `CanMehCalibrate` sizes it per pair.
- Reported numbers must come from 24-core Xeon `xcnc` or `xcnd` nodes. Nothing else may run on the node, and a local timing is not comparable.
- Two searches need two work directories, and at most four should run at once against the endpoint. Do not rebuild a checkout while jobs use it, and do not run two `cmake --build` in one checkout.
- `CanMehSearch` writes `best.patch` only at the end. After a Slurm kill, recover it from `results.jsonl` and `patches/gen_N.patch`.

## Cluster

Reach it with `ssh -o BatchMode=yes xlogin` over a shared connection the user authenticated by hand; `BatchMode` makes a dead connection fail rather than hang. Allocate with `--constraint="xcnc|xcnd"` (quoted) and `--mem=0 --cpus-per-task=24`, because `--exclusive` is refused. `sacct` and `seff` are broken, so use `scontrol show job`. `--partition=long` has been landing in `normal` (3-hour limit), so read `Partition=` before relying on more. **Submitting a search** in the README has the full `sbatch` command.
