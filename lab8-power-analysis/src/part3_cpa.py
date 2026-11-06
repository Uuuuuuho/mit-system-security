"""Part 3 -- Correlation power analysis.

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
    """traces (n, S), hyps (n, G)  ->  (G, S) float array with
    rho[g, s] = corr(hyps[:, g], traces[:, s]).  Vectorize: no Python loop over g or s."""
    T = np.asarray(traces, dtype=np.float64)
    H = np.asarray(hyps, dtype=np.float64)
    # TODO: center both matrices (subtract the column means), then
    #   rho = (H^T T) / (||H_g|| * ||T_s||)   with the norms taken column-wise.
    return np.zeros((H.shape[1], T.shape[1]))


def hyp_hw_sbox(pts, byte):
    """(n, 256) predicted leakage HW(SBOX[p ^ g]) for every guess g."""
    x = pts[:, byte].astype(np.intp)[:, None] ^ np.arange(256)[None, :]
    return HW8[SBOX[x]]


def hyp_hd_sbox(pts, byte):
    """(n, 256) predicted leakage for the Hamming-distance device, for every guess g."""
    # TODO (Exercise 3-3): in the HD model an instruction leaks the number of bits it flips in
    # its destination register.  Read aes_firmware in powersim/firmware.py: which register
    # holds which value right before and right after the S-box lookup?
    return np.zeros((pts.shape[0], 256))


# ---------------------------------------------------------------------------
# Exercises 3-2 / 3-3: full key
# ---------------------------------------------------------------------------
def cpa_recover_key(traces, pts, model="hw"):
    """Return the 16-byte key.  model 'hw': Hamming weight of the S-box output (Exercise 3-2);
    'hd': your Hamming-distance hypothesis (Exercise 3-3)."""
    hyp = hyp_hw_sbox if model == "hw" else hyp_hd_sbox
    key = np.zeros(16, dtype=np.uint8)
    # TODO: for each byte, correlate all 256 hypotheses with all samples and keep the guess
    # with the largest |rho| anywhere in the trace.
    return key


# ---------------------------------------------------------------------------
# Exercise 3-4: key rank versus number of traces
# ---------------------------------------------------------------------------
def rank_curve(traces, pts, key, ns):
    """For each n in ns (increasing), run CPA (HW of the S-box output, score = max over samples
    of |rho|) on the FIRST n traces and return an int array (len(ns), 16): the rank of the
    correct byte key[b] among the 256 guesses (0 = best = number of guesses scoring higher)."""
    ranks = np.zeros((len(ns), 16), dtype=int)
    # TODO: the straightforward way recomputes CPA for every n (fine for a first version).
    # Faster: keep running sums (sum t, sum t^2, sum h, sum h^2, sum h*t) and update them with
    # the traces between consecutive values of n; Pearson only needs these five sums.
    return ranks


def traces_needed(ranks, ns):
    """Smallest n in ns such that every byte has rank 0 at n and at every larger n in ns
    (None if that never happens)."""
    # TODO
    return None


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
        print(f"recovered {bytes(np.asarray(k, dtype=np.uint8).tolist()).hex()}\n"
              f"correct   {bytes(dev._key).hex()}  ({int((np.asarray(k) == dev._key).sum())}/16 bytes)")
    else:
        noises = ([float(x) for x in a.noises.split(",")] if a.noises
                  else [4.0 if a.noise is None else a.noise])
        nmax = 1500 if a.n is None else a.n
        series, labels = [], []
        for sg in noises:
            dev = AESDevice(seed=a.seed, noise=sg)
            ts = dev.capture(nmax)
            ns = default_ns(nmax)
            ranks = np.asarray(rank_curve(ts.traces, ts.pts, dev._key, ns))
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
