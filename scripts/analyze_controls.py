#!/usr/bin/env python3
"""Summarize scripts/controls.sh: the baselines, then each compiler setting and patch.

Usage, from the repository root: scripts/analyze_controls.py <output-directory>
Reads init-*.json and verdicts.jsonl there, and prints a table. A ratio below 1.0
means faster than the -O2 library.
"""
import glob
import json
import os
import statistics
import sys


def main():
    root = sys.argv[1]
    print("baselines (milliseconds describe this node only)")
    for path in sorted(glob.glob(os.path.join(root, "init-*.json"))):
        label = os.path.basename(path)[len("init-"):-len(".json")]
        with open(path) as handle:
            init = json.load(handle)
        print(f"  {label:<11}{init.get('baseline_ms', float('nan')):>10.3f} ms   "
              f"null {'clean' if init.get('null_ok') else 'DIRTY'}   "
              f"null ratio {init.get('null_ratio', float('nan')):.4f}")

    with open(os.path.join(root, "verdicts.jsonl")) as handle:
        rows = [json.loads(line) for line in handle if line.strip()]
    groups = {}
    for row in rows:
        groups.setdefault((row["kind"], row["name"]), []).append(row)

    print()
    print(f"{'kind':<7}{'name':<12}{'trials':>7}{'faster':>7}{'unstable':>9}"
          f"{'ratio':>9}{'speedup':>9}{'lowest':>9}{'highest':>9}")
    for (kind, name), members in groups.items():
        ratios = [row["ratio"] for row in members if row.get("measured")]
        if not ratios:
            print(f"{kind:<7}{name:<12}{len(members):>7}  no trial was measured: "
                  f"{members[0].get('why', '')}")
            continue
        speedups = sorted((1 / ratio - 1) * 100 for ratio in ratios)
        print(f"{kind:<7}{name:<12}{len(ratios):>7}"
              f"{sum(1 for row in members if row.get('faster')):>7}"
              f"{sum(1 for row in members if row.get('unstable')):>9}"
              f"{statistics.median(ratios):>9.4f}{statistics.median(speedups):>8.2f}%"
              f"{speedups[0]:>8.2f}%{speedups[-1]:>8.2f}%")


if __name__ == "__main__":
    main()
