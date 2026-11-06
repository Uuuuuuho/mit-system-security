"""fpdata.py - shared helpers for lab1 (trace files, datasets, reports). numpy only.

Dataset file (written by tools/collect.py, read by part2/eval.py and tools/plot.py):
    <name>.npz with
        X      float64 array (n_traces, n_windows)   one row per trace, one column per window
        y      int64 array (n_traces,)               label = index into `sites`
        sites  str array (n_sites,)                  site names
        meta   str (JSON)                            collector, P, T, layout, buffer, machine ...
"""
import json
import os

import numpy as np

try:  # scikit-learn is optional everywhere in this lab
    import sklearn  # noqa: F401
    HAVE_SKLEARN = True
except Exception:  # pragma: no cover - depends on the machine
    HAVE_SKLEARN = False


def read_trace(path):
    """Parse one fp-trace v1 text file -> (header dict, counts as np.ndarray)."""
    header = {}
    counts = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                for tok in line[1:].split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        header[k] = v
                continue
            counts.append(int(line))
    return header, np.asarray(counts, dtype=np.float64)


def save_dataset(path, X, y, sites, meta):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    np.savez_compressed(path, X=np.asarray(X, dtype=np.float64), y=np.asarray(y, dtype=np.int64),
                        sites=np.asarray(sites), meta=json.dumps(meta))


def load_dataset(path):
    """-> X, y, site names (list of str), meta (dict)."""
    d = np.load(path, allow_pickle=False)
    return d["X"], d["y"], [str(s) for s in d["sites"]], json.loads(str(d["meta"]))


def confusion(y_true, y_pred, n_classes):
    m = np.zeros((n_classes, n_classes), dtype=int)
    for t, p in zip(y_true, y_pred):
        m[int(t), int(p)] += 1
    return m


def classification_report(y_true, y_pred, names):
    """Per-class precision / recall / F1 table (same layout as sklearn's) + confusion matrix."""
    y_true = np.asarray(y_true)
    y_pred = np.asarray(y_pred)
    k = len(names)
    m = confusion(y_true, y_pred, k)
    w = max(12, max(len(n) for n in names) + 2)
    lines = [f"{'':>{w}} {'precision':>9} {'recall':>9} {'f1':>9} {'support':>9}"]
    for i, n in enumerate(names):
        tp = m[i, i]
        prec = tp / m[:, i].sum() if m[:, i].sum() else 0.0
        rec = tp / m[i, :].sum() if m[i, :].sum() else 0.0
        f1 = 2 * prec * rec / (prec + rec) if prec + rec else 0.0
        lines.append(f"{n:>{w}} {prec:9.2f} {rec:9.2f} {f1:9.2f} {m[i, :].sum():9d}")
    acc = float(np.mean(y_true == y_pred)) if len(y_true) else 0.0
    lines.append(f"{'accuracy':>{w}} {'':>9} {'':>9} {acc:9.2f} {len(y_true):9d}")
    lines.append("")
    lines.append("confusion matrix (rows = true site, columns = predicted site):")
    short = [n[:6] for n in names]
    lines.append(f"{'':>{w}} " + " ".join(f"{s:>6}" for s in short))
    for i, n in enumerate(names):
        lines.append(f"{n:>{w}} " + " ".join(f"{v:6d}" for v in m[i]))
    return "\n".join(lines)
