"""Part 2 -- Differential power analysis (difference of means).  (REFERENCE SOLUTION)

See README.md, Part 2.  Run directly to attack a fresh device:

    python3 src/part2_dpa.py [--seed N] [--n 2000] [--noise 2.0]

Target: the output of the first-round S-box, y = SBOX[p_i ^ k_i], one key byte at a time.
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))   # for `import powersim`
from powersim import SBOX   # noqa: E402


# ---------------------------------------------------------------------------
# Exercise 2-1: difference-of-means traces for all 256 guesses of one key byte
# ---------------------------------------------------------------------------
def dom_traces(traces, pts, byte, bit=0):
    """traces (n, S) float, pts (n, 16) uint8.

    For every guess g of key byte `byte`, split the traces by the predicted selection bit
        D_g = bit `bit` of SBOX[pts[:, byte] ^ g]
    and return the (256, S) array  mean(traces | D_g = 1) - mean(traces | D_g = 0)."""
    traces = np.asarray(traces, dtype=np.float64)
    n = traces.shape[0]
    x = pts[:, byte].astype(np.intp)[:, None] ^ np.arange(256)[None, :]       # (n, 256)
    sel = ((SBOX[x] >> bit) & 1).astype(np.float64)                           # (n, 256)
    n1 = sel.sum(axis=0)
    n0 = n - n1
    s1 = sel.T @ traces                                                       # (256, S)
    s0 = traces.sum(axis=0)[None, :] - s1
    with np.errstate(divide="ignore", invalid="ignore"):
        d = s1 / n1[:, None] - s0 / n0[:, None]
    return np.nan_to_num(d)


# ---------------------------------------------------------------------------
# Exercise 2-2: recover the full key
# ---------------------------------------------------------------------------
def dpa_recover_key(traces, pts):
    """Return the 16-byte key as a uint8 array (or bytes / list of ints).

    Multi-bit DPA: for each guess, add up the peak |difference| obtained with each of the 8
    selection bits (each bit is an independent single-bit DPA; their ghost peaks do not line up,
    the correct guess's peaks do)."""
    key = np.zeros(16, dtype=np.uint8)
    for b in range(16):
        score = np.zeros(256)
        for bit in range(8):
            score += np.abs(dom_traces(traces, pts, b, bit)).max(axis=1)
        key[b] = int(np.argmax(score))
    return key


if __name__ == "__main__":
    import argparse
    from powersim import AESDevice
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--n", type=int, default=2000)
    ap.add_argument("--noise", type=float, default=2.0)
    a = ap.parse_args()
    dev = AESDevice(seed=a.seed, noise=a.noise)
    ts = dev.capture(a.n)
    d = dom_traces(ts.traces, ts.pts, 0, 0)
    g = int(np.argmax(np.abs(d).max(axis=1)))
    s = int(np.argmax(np.abs(d[g])))
    print(f"byte 0, bit 0: best guess {g:#04x} peak {d[g, s]:+.3f} at sample {s} ({ts.tags[s]})")
    k = dpa_recover_key(ts.traces, ts.pts)
    print(f"recovered {bytes(k).hex()}\ncorrect   {bytes(dev._key).hex()}  "
          f"({int((k == dev._key).sum())}/16 bytes)")
