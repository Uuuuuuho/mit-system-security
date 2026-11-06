"""Part 3 -- Correlation power analysis.  (REFERENCE SOLUTION)

See README.md, Part 3.  Run directly:

    python3 src/part3_cpa.py [--seed N] [--noise 3.0] [--n 400] [--model hw|hd]
    python3 src/part3_cpa.py --curve [--noises 1,2,4] [--n 1500] [--png out/rank_curve.png]
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))   # for `import powersim`
from powersim import SBOX, HW8   # noqa: E402


# ---------------------------------------------------------------------------
# Exercise 3-1: Pearson correlation between hypotheses and traces
# ---------------------------------------------------------------------------
def pearson(traces, hyps):
    """traces (n, S), hyps (n, G)  ->  (G, S) array of correlation coefficients
    rho[g, s] = corr(hyps[:, g], traces[:, s]).  Constant columns give 0."""
    T = np.asarray(traces, dtype=np.float64)
    H = np.asarray(hyps, dtype=np.float64)
    T = T - T.mean(axis=0)
    H = H - H.mean(axis=0)
    num = H.T @ T
    den = np.sqrt((H * H).sum(axis=0))[:, None] * np.sqrt((T * T).sum(axis=0))[None, :]
    with np.errstate(divide="ignore", invalid="ignore"):
        r = np.where(den > 0, num / np.where(den > 0, den, 1), 0.0)
    return r


def hyp_hw_sbox(pts, byte):
    """(n, 256) predicted leakage HW(SBOX[p ^ g]) for every guess g."""
    x = pts[:, byte].astype(np.intp)[:, None] ^ np.arange(256)[None, :]
    return HW8[SBOX[x]]


def hyp_hd_sbox(pts, byte):
    """(n, 256) predicted leakage HD(x, SBOX[x]) = HW(x ^ SBOX[x]) with x = p ^ g: the register
    that holds the S-box input is overwritten by the S-box output."""
    x = pts[:, byte].astype(np.intp)[:, None] ^ np.arange(256)[None, :]
    return HW8[x ^ SBOX[x]]


# ---------------------------------------------------------------------------
# Exercises 3-2 / 3-3: full key
# ---------------------------------------------------------------------------
def cpa_recover_key(traces, pts, model="hw"):
    """Return the 16-byte key.  model 'hw': HW of the S-box output; 'hd': HD between S-box input
    and output."""
    hyp = hyp_hw_sbox if model == "hw" else hyp_hd_sbox
    key = np.zeros(16, dtype=np.uint8)
    for b in range(16):
        r = pearson(traces, hyp(pts, b))
        key[b] = int(np.argmax(np.abs(r).max(axis=1)))
    return key


# ---------------------------------------------------------------------------
# Exercise 3-4: key rank versus number of traces
# ---------------------------------------------------------------------------
def byte_ranks(scores, key):
    """scores (16, 256) -> rank of the correct byte for each of the 16 bytes (0 = best)."""
    return np.array([int((scores[b] > scores[b, key[b]]).sum()) for b in range(16)])


def rank_curve(traces, pts, key, ns):
    """For each n in ns (increasing), run CPA (HW of S-box output, max |rho| over samples) on the
    first n traces and return an int array (len(ns), 16) with the rank of the correct key byte.

    Incremental: running sums over trace blocks, so the total cost is one pass over the data."""
    T = np.asarray(traces, dtype=np.float64)
    ns = [int(n) for n in ns]
    S = T.shape[1]
    out = np.zeros((len(ns), 16), dtype=int)
    hyps = [hyp_hw_sbox(pts, b).astype(np.float64) for b in range(16)]
    st, st2 = np.zeros(S), np.zeros(S)
    sh = [np.zeros(256) for _ in range(16)]
    sh2 = [np.zeros(256) for _ in range(16)]
    sht = [np.zeros((256, S)) for _ in range(16)]
    prev = 0
    for i, n in enumerate(ns):
        blk = T[prev:n]
        st += blk.sum(axis=0)
        st2 += (blk * blk).sum(axis=0)
        scores = np.zeros((16, 256))
        for b in range(16):
            h = hyps[b][prev:n]
            sh[b] += h.sum(axis=0)
            sh2[b] += (h * h).sum(axis=0)
            sht[b] += h.T @ blk
            num = n * sht[b] - sh[b][:, None] * st[None, :]
            den = np.sqrt(np.maximum(n * sh2[b] - sh[b] ** 2, 0))[:, None] * \
                np.sqrt(np.maximum(n * st2 - st ** 2, 0))[None, :]
            with np.errstate(divide="ignore", invalid="ignore"):
                r = np.where(den > 0, num / np.where(den > 0, den, 1), 0.0)
            scores[b] = np.abs(r).max(axis=1)
        out[i] = byte_ranks(scores, key)
        prev = n
    return out


def traces_needed(ranks, ns):
    """Smallest n in ns such that every byte has rank 0 at n and at every larger n in ns
    (None if that never happens)."""
    ok = (np.asarray(ranks) == 0).all(axis=1)
    if not ok[-1]:
        return None
    i = len(ok) - 1
    while i > 0 and ok[i - 1]:
        i -= 1
    return int(ns[i])


def default_ns(nmax):
    ns = sorted(set(int(round(x)) for x in np.geomspace(5, nmax, 24)))
    return [n for n in ns if n >= 5]


if __name__ == "__main__":
    import argparse
    from powersim import AESDevice
    from powersim.stats import save_plot
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--noise", type=float, default=None)
    ap.add_argument("--n", type=int, default=None)
    ap.add_argument("--model", choices=("hw", "hd"), default="hw")
    ap.add_argument("--curve", action="store_true", help="key rank vs number of traces")
    ap.add_argument("--noises", default=None, help="comma-separated noise levels for --curve, e.g. 1,2,4")
    ap.add_argument("--png", default="out/rank_curve.png")
    a = ap.parse_args()
    if not a.curve:
        dev = AESDevice(seed=a.seed, model=a.model, noise=3.0 if a.noise is None else a.noise)
        ts = dev.capture(400 if a.n is None else a.n)
        k = cpa_recover_key(ts.traces, ts.pts, model=a.model)
        print(f"recovered {bytes(k).hex()}\ncorrect   {bytes(dev._key).hex()}  "
              f"({int((k == dev._key).sum())}/16 bytes)")
    else:
        noises = ([float(x) for x in a.noises.split(",")] if a.noises
                  else [4.0 if a.noise is None else a.noise])
        nmax = 1500 if a.n is None else a.n
        series, labels = [], []
        for sg in noises:
            dev = AESDevice(seed=a.seed, noise=sg)
            ts = dev.capture(nmax)
            ns = default_ns(nmax)
            ranks = rank_curve(ts.traces, ts.pts, dev._key, ns)
            print(f"noise {sg}:  n   mean-rank  bytes-at-rank-0  log2(prod(rank+1))")
            for n, r in zip(ns, ranks):
                print(f"      {n:6d}  {r.mean():8.1f}  {int((r == 0).sum()):4d}   {np.log2(r + 1).sum():6.1f}")
            print(f"traces needed: {traces_needed(ranks, ns)}")
            series.append((ns, ranks.mean(axis=1)))
            labels.append(f"noise {sg}")
        os.makedirs(os.path.dirname(a.png) or ".", exist_ok=True)
        if save_plot(a.png, series, title="CPA: mean rank of the correct key byte", xlabel="traces",
                     ylabel="mean byte rank", logx=True, labels=labels):
            print(f"wrote {a.png}")
        else:
            print("(matplotlib not available: table only)")
