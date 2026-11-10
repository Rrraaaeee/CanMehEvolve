#!/usr/bin/env python3
"""Summarize Phase 0: rate, bias and autocorrelation per calibration pair.

Usage, from the repository root: scripts/analyze_phase0.py [phase0-directory]
Writes summary.json into that directory and prints a table.
"""
import glob
import json
import math
import os
import statistics
import sys

PAIRS = ["null", "faster-1-percent", "faster-2-percent", "faster-5-percent",
         "faster-10-percent", "slower-10-percent"]
CALIBRATION_ROOT = "build/calibration"


def binomial_tail_at_least(k, n, p):
    return sum(math.comb(n, i) * p**i * (1 - p)**(n - i) for i in range(k, n + 1))


def bisect(function, target):
    """The p in [0, 1] where the increasing function reaches target."""
    low, high = 0.0, 1.0
    for _ in range(200):
        middle = (low + high) / 2
        if function(middle) < target:
            low = middle
        else:
            high = middle
    return (low + high) / 2


def clopper_pearson(k, n, confidence=0.95):
    """The exact binomial interval for k successes in n trials."""
    tail = (1 - confidence) / 2
    lower = 0.0 if k == 0 else bisect(lambda p: binomial_tail_at_least(k, n, p), tail)
    upper = 1.0 if k == n else bisect(lambda p: binomial_tail_at_least(k + 1, n, p), 1 - tail)
    return lower, upper


def lag1_autocorrelation(series):
    mean = statistics.fmean(series)
    denominator = sum((x - mean) ** 2 for x in series)
    if denominator == 0:
        return 0.0
    return sum((series[i] - mean) * (series[i + 1] - mean)
               for i in range(len(series) - 1)) / denominator


def true_ratio(pair):
    with open(os.path.join(CALIBRATION_ROOT, pair, "meta.json")) as handle:
        units = json.load(handle)["calibration"]
    return units["candidate_units"] / units["reference_units"]


def summarize(directory, pair, verdicts):
    rows = [v for v in verdicts if v["pair"] == pair]
    measured = [v for v in rows if v["measured"]]
    called_faster = sum(1 for v in measured if v["faster"])
    truth = true_ratio(pair)

    per_trial_medians = []
    every_process = []
    for path in sorted(glob.glob(os.path.join(directory, "trials", pair + "-*.json"))):
        with open(path) as handle:
            reply = json.load(handle)
        if not reply.get("ok"):
            continue
        values = [lag1_autocorrelation(p["candidate_ns"]) for p in reply["timings"]["process"]]
        every_process.extend(values)
        per_trial_medians.append(statistics.median(values))

    ratios = [v["ratio"] for v in measured]
    low, high = clopper_pearson(called_faster, len(measured)) if measured else (None, None)
    return {
        "pair": pair,
        "true_ratio": truth,
        "trials": len(rows),
        "measured": len(measured),
        "called_faster": called_faster,
        "rate": called_faster / len(measured) if measured else None,
        "rate_ci95": [low, high],
        "median_ratio": statistics.median(ratios) if ratios else None,
        "mean_bias": statistics.fmean(r - truth for r in ratios) if ratios else None,
        "median_bias": statistics.median(r - truth for r in ratios) if ratios else None,
        "largest_absolute_error": max(abs(r - truth) for r in ratios) if ratios else None,
        "median_spread": statistics.median(v["spread"] for v in measured) if measured else None,
        "unstable_trials": sum(1 for v in measured if v["unstable"]),
        "load_range": [min(v["load"] for v in rows), max(v["load"] for v in rows)] if rows else None,
        "lag1_median_of_trial_medians": statistics.median(per_trial_medians) if per_trial_medians else None,
        "lag1_process_range": [min(every_process), max(every_process)] if every_process else None,
    }


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "results/phase0"
    with open(os.path.join(directory, "verdicts.jsonl")) as handle:
        verdicts = [json.loads(line) for line in handle if line.strip()]
    summary = [summarize(directory, pair, verdicts) for pair in PAIRS]
    with open(os.path.join(directory, "summary.json"), "w") as handle:
        json.dump(summary, handle, indent=2)
        handle.write("\n")

    print(f"{'pair':<20}{'true':>8}{'n':>4}{'faster':>7}{'rate':>7}{'95% CI':>16}"
          f"{'median':>9}{'bias':>9}{'spread':>8}{'lag1':>7}{'load':>13}")
    for row in summary:
        if not row["measured"]:
            print(f"{row['pair']:<20}no measured trials")
            continue
        low, high = row["rate_ci95"]
        print(f"{row['pair']:<20}{row['true_ratio']:>8.4f}{row['measured']:>4}"
              f"{row['called_faster']:>7}{row['rate']:>7.2f}{f'{low:.3f}-{high:.3f}':>16}"
              f"{row['median_ratio']:>9.4f}{row['mean_bias']:>+9.4f}{row['median_spread']:>8.3f}"
              f"{row['lag1_median_of_trial_medians']:>+7.2f}"
              f"{f'{row['load_range'][0]:.0f}-{row['load_range'][1]:.0f}':>13}")


if __name__ == "__main__":
    main()
