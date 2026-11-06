"""Small statistics/plotting helpers that are *not* exercises.

welch_t          Welch's t statistic per sample (TVLA-style leakage detection)
moving_average   boxcar smoothing along the last axis
sparkline        one-line text rendering of a trace (for terminals without matplotlib)
"""

import numpy as np


def welch_t(a, b):
    """Welch's t statistic between two groups of traces, per sample.
    a: (na, S), b: (nb, S) -> (S,).  |t| > 4.5 in two independent experiments is the usual
    TVLA criterion for 'this sample leaks at first order'."""
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    va = a.var(axis=0, ddof=1) / a.shape[0]
    vb = b.var(axis=0, ddof=1) / b.shape[0]
    den = np.sqrt(va + vb)
    diff = a.mean(axis=0) - b.mean(axis=0)
    with np.errstate(divide="ignore", invalid="ignore"):
        t = np.where(den > 0, diff / np.where(den > 0, den, 1), np.where(diff == 0, 0.0, np.inf))
    return t


def moving_average(x, w, axis=-1):
    """Centered boxcar average of width w (same length as x; edges use a shorter window)."""
    x = np.asarray(x, dtype=np.float64)
    x = np.moveaxis(x, axis, -1)
    c = np.cumsum(np.pad(x, [(0, 0)] * (x.ndim - 1) + [(1, 0)]), axis=-1)
    n = x.shape[-1]
    lo = np.clip(np.arange(n) - w // 2, 0, n)
    hi = np.clip(np.arange(n) - w // 2 + w, 0, n)
    out = (c[..., hi] - c[..., lo]) / (hi - lo)
    return np.moveaxis(out, -1, axis)


_BLOCKS = " .:-=+*#%@"


def sparkline(trace, width=100, lo=None, hi=None):
    """Render a 1-D trace as one line of `width` characters (mean of each bucket)."""
    t = np.asarray(trace, dtype=np.float64)
    nb = min(width, len(t))
    edges = np.linspace(0, len(t), nb + 1).astype(int)
    v = np.array([t[edges[i]:max(edges[i] + 1, edges[i + 1])].mean() for i in range(nb)])
    lo = v.min() if lo is None else lo
    hi = v.max() if hi is None else hi
    q = np.clip(((v - lo) / max(hi - lo, 1e-9) * (len(_BLOCKS) - 1)).round().astype(int), 0, len(_BLOCKS) - 1)
    return "".join(_BLOCKS[i] for i in q)


def save_plot(path, series, title="", xlabel="sample", ylabel="", logx=False, labels=None):
    """Save a line plot with matplotlib if it is installed.  Returns True on success."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception:
        return False
    fig, ax = plt.subplots(figsize=(10, 4))
    for i, s in enumerate(series):
        if isinstance(s, tuple):
            ax.plot(*s, label=None if labels is None else labels[i], linewidth=0.8)
        else:
            ax.plot(s, label=None if labels is None else labels[i], linewidth=0.8)
    if logx:
        ax.set_xscale("log")
    ax.set_title(title)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    if labels:
        ax.legend()
    fig.tight_layout()
    fig.savefig(path, dpi=120)
    plt.close(fig)
    return True
