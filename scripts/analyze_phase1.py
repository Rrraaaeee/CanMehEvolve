#!/usr/bin/env python3
"""Summarize Phase 1: each run, then the median and range per arm (controls C3 and C4).

Usage, from the repository root: scripts/analyze_phase1.py [phase1-directory]
Expects <directory>/<arm>-<seed>/. Writes summary.json there and prints a table.
"""
import csv
import glob
import json
import os
import statistics
import sys


def last_credits_row(directory):
    with open(os.path.join(directory, "credits.csv")) as handle:
        rows = list(csv.DictReader(handle))
    return rows[-1] if rows else None


def summarize_run(directory):
    with open(os.path.join(directory, "results.jsonl")) as handle:
        records = [json.loads(line) for line in handle if line.strip()]
    with open(os.path.join(directory, "models.csv")) as handle:
        models = sorted({row["model"] for row in csv.DictReader(handle)})
    credits = last_credits_row(directory)
    measured = [r for r in records if r.get("ok")]
    with open(os.path.join(directory, "environment.txt")) as handle:
        environment = handle.read()
    return {
        "run": os.path.basename(directory),
        "calls": int(credits["calls"]) if credits else 0,
        "tokens": int(credits["total_tokens"]) if credits else 0,
        "elapsed_s": float(credits["elapsed_s"]) if credits else 0.0,
        "best_confirmed_speedup": float(credits["best_confirmed_speedup"]) if credits else 1.0,
        "best_retained_speedup": float(credits["best_retained_speedup"]) if credits else 1.0,
        "records": len(records),
        "measured": len(measured),
        "unstable": sum(1 for r in measured if r.get("unstable")),
        "improved_on_parent": sum(1 for r in measured if r.get("improved_on_parent")),
        "rejected_stages": sorted({r["stage"] for r in records if r.get("stage")}),
        "models_answered": models,
        "null_clean": "null run: clean" in environment,
        "profiled": "profile: used" in environment,
    }


def spread(values):
    return {"median": statistics.median(values), "min": min(values), "max": max(values)}


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "results/phase1"
    runs = [summarize_run(os.path.dirname(path))
            for path in sorted(glob.glob(os.path.join(root, "*-*", "credits.csv")))]
    arms = {}
    for run in runs:
        arm = run["run"].rsplit("-", 1)[0]
        arms.setdefault(arm, []).append(run)
    per_arm = {arm: {"seeds": len(members),
                     "calls": sum(r["calls"] for r in members),
                     "tokens": sum(r["tokens"] for r in members),
                     "best_confirmed_speedup": spread([r["best_confirmed_speedup"] for r in members]),
                     "best_retained_speedup": spread([r["best_retained_speedup"] for r in members])}
               for arm, members in arms.items()}
    with open(os.path.join(root, "summary.json"), "w") as handle:
        json.dump({"runs": runs, "arms": per_arm}, handle, indent=2)
        handle.write("\n")

    print(f"{'run':<14}{'calls':>6}{'meas':>6}{'unst':>6}{'impr':>6}{'conf':>9}{'kept':>9}"
          f"{'tokens':>9}{'s':>8}  null   profile  model")
    for run in runs:
        print(f"{run['run']:<14}{run['calls']:>6}{run['measured']:>6}{run['unstable']:>6}"
              f"{run['improved_on_parent']:>6}{run['best_confirmed_speedup']:>9.4f}"
              f"{run['best_retained_speedup']:>9.4f}{run['tokens']:>9}{run['elapsed_s']:>8.0f}  "
              f"{'clean' if run['null_clean'] else 'DIRTY':<7}{'yes' if run['profiled'] else 'NONE':<9}"
              f"{','.join(run['models_answered'])}")
    print()
    for arm, row in per_arm.items():
        confirmed, kept = row["best_confirmed_speedup"], row["best_retained_speedup"]
        print(f"{arm}: {row['seeds']} seeds, {row['calls']} calls; confirmed speedup median "
              f"{confirmed['median']:.4f} range {confirmed['min']:.4f}-{confirmed['max']:.4f}; "
              f"retained median {kept['median']:.4f} range {kept['min']:.4f}-{kept['max']:.4f}")


if __name__ == "__main__":
    main()
