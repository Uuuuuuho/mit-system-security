#!/usr/bin/env python3
"""plot.py - plot a few traces per site (Discussion 2-2) and print an ASCII overview.

    python3 tools/plot.py traces/sweep.npz [--per-site 3] [--out out/sweep_traces.png]

matplotlib is optional: without it (or with --ascii) only the ASCII strips are printed.
Each ASCII strip is one trace; darker characters = more work done in that window.
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fpdata  # noqa: E402

RAMP = " .:-=+*#%@"


def ascii_strip(row, width=100):
    row = np.asarray(row, dtype=float)
    if len(row) > width:  # average down to `width` columns
        edges = np.linspace(0, len(row), width + 1).astype(int)
        row = np.array([row[a:b].mean() for a, b in zip(edges[:-1], edges[1:])])
    top = np.percentile(row, 95) or 1.0
    return "".join(RAMP[min(len(RAMP) - 1, int(v / top * (len(RAMP) - 1)))] for v in row)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dataset")
    ap.add_argument("--per-site", type=int, default=3)
    ap.add_argument("--out", default=None, help="PNG path (default: out/<dataset name>_traces.png)")
    ap.add_argument("--ascii", action="store_true", help="ASCII only, no matplotlib")
    a = ap.parse_args()

    X, y, names, meta = fpdata.load_dataset(a.dataset)
    P = float(meta.get("P_ms", 1))
    print(f"{os.path.basename(a.dataset)}: collector={meta.get('collector')} P={P}ms T={meta.get('T_s')}s "
          f"layout={meta.get('layout')} buf={meta.get('buf')}")
    for k, n in enumerate(names):
        for r in np.flatnonzero(y == k)[: a.per_site]:
            print(f"{n:>8} |{ascii_strip(X[r])}|")
    if a.ascii:
        return
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception:
        print("(matplotlib not available: ASCII only)")
        return
    out = a.out or os.path.join("out", os.path.splitext(os.path.basename(a.dataset))[0] + "_traces.png")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    fig, axes = plt.subplots(len(names), 1, figsize=(10, 1.6 * len(names)), sharex=True)
    t = np.arange(X.shape[1]) * P / 1000.0
    for k, (ax, n) in enumerate(zip(np.atleast_1d(axes), names)):
        for r in np.flatnonzero(y == k)[: a.per_site]:
            ax.plot(t, X[r], lw=0.8)
        ax.set_ylabel(n, rotation=0, ha="right", va="center")
        ax.grid(alpha=0.3)
    np.atleast_1d(axes)[-1].set_xlabel("time (s)")
    fig.suptitle(f"{meta.get('collector')} collector: work per {P:g} ms window, {a.per_site} traces per site")
    fig.tight_layout()
    fig.savefig(out, dpi=120)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
