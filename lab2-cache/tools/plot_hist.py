#!/usr/bin/env python3
"""Plot Part 1 latency histograms:  ./bin/part1 --csv out.csv && python3 tools/plot_hist.py out.csv [out.png]

Uses matplotlib if available (writes a PNG); otherwise prints a text histogram per level."""
import csv
import sys
from collections import defaultdict


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    data = defaultdict(list)
    with open(sys.argv[1]) as f:
        for row in csv.DictReader(f):
            data[row["level"]].append(int(row["latency"]))
    levels = [l for l in ("L1", "L2", "LLC", "DRAM") if l in data]
    allv = sorted(v for l in levels for v in data[l])
    hi = allv[int(0.995 * (len(allv) - 1))]  # clip interrupt outliers
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        width = max(1, hi // 40)
        for l in levels:
            print(f"== {l}")
            counts = defaultdict(int)
            for v in data[l]:
                if v <= hi:
                    counts[v // width] += 1
            top = max(counts.values())
            for b in sorted(counts):
                print(f"{b * width:6d} {'#' * max(1, 60 * counts[b] // top)} {counts[b]}")
        return
    out = sys.argv[2] if len(sys.argv) > 2 else sys.argv[1].rsplit(".", 1)[0] + ".png"
    fig, ax = plt.subplots(figsize=(8, 4))
    bins = range(0, hi + 2, max(1, hi // 120))
    for l in levels:
        ax.hist([v for v in data[l] if v <= hi], bins=bins, alpha=0.6, label=l)
    ax.set_xlabel("probe latency (ticks)")
    ax.set_ylabel("samples")
    ax.set_yscale("log")
    ax.legend()
    ax.set_title("lab2-cache Part 1: load latency by level")
    fig.tight_layout()
    fig.savefig(out, dpi=120)
    print("wrote", out)


if __name__ == "__main__":
    main()
