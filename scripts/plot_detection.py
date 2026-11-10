#!/usr/bin/env python3
"""Draw the report's figure: the rule's detection rate at each true effect size.

Usage, from the repository root: scripts/plot_detection.py [phase0-directory] [output.pdf]
Reads summary.json from that directory (default results/phase0) and writes the
figure, by default to submission/figures/detection.pdf, with a .png beside it.
"""
import json
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

SUMMARY = os.path.join(sys.argv[1] if len(sys.argv) > 1 else "results/phase0", "summary.json")
OUTPUT = sys.argv[2] if len(sys.argv) > 2 else "submission/figures/detection.pdf"
INK = "#1F4E79"
MUTED = "#6B7280"
GRID = "#E5E7EB"


def main():
    with open(SUMMARY) as handle:
        rows = [row for row in json.load(handle)
                if row["pair"].startswith("faster") and row["rate"] is not None]
    rows.sort(key=lambda row: row["true_ratio"], reverse=True)
    trials = max(row["measured"] for row in rows)

    effects = [(1 / row["true_ratio"] - 1) * 100 for row in rows]
    rates = [row["rate"] for row in rows]
    below = [row["rate"] - row["rate_ci95"][0] for row in rows]
    above = [row["rate_ci95"][1] - row["rate"] for row in rows]

    plt.rcParams.update({"font.family": "serif", "font.size": 8, "pdf.fonttype": 42})
    figure, axis = plt.subplots(figsize=(3.3, 2.0))
    axis.axhline(0.9, color=MUTED, linewidth=0.8, linestyle=(0, (4, 3)), zorder=1)
    axis.text(0.4, 0.93, "README's bar: 9 of 10", color=MUTED, fontsize=7, va="bottom")
    axis.errorbar(effects, rates, yerr=[below, above], color=INK, linewidth=1.2, marker="o",
                  markersize=4, capsize=2.5, elinewidth=0.8, zorder=3)

    axis.set_xlabel("True speedup of the calibration pair (%)")
    axis.set_ylabel(f"Share of {trials} trials\ncalled faster")
    axis.set_xlim(0, 11)
    axis.set_ylim(-0.03, 1.08)
    axis.set_xticks([1, 2, 5, 10])
    axis.set_yticks([0, 0.25, 0.5, 0.75, 1.0])
    axis.grid(axis="y", color=GRID, linewidth=0.6, zorder=0)
    axis.set_axisbelow(True)
    for side in ("top", "right"):
        axis.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        axis.spines[side].set_color(MUTED)
    axis.tick_params(colors=MUTED, length=2, labelcolor="black")

    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    figure.tight_layout(pad=0.3)
    figure.savefig(OUTPUT)
    figure.savefig(OUTPUT.replace(".pdf", ".png"), dpi=200)


if __name__ == "__main__":
    main()
