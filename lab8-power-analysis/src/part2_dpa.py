"""Part 2 -- Differential power analysis (difference of means).

See README.md, Part 2.  Run directly to attack a fresh device:

    python3 src/part2_dpa.py [--seed N] [--n 2000] [--noise 2.0]

Target: the output of the first-round S-box, y = SBOX[p_i ^ k_i], one key byte at a time.
The device is an 8-bit AES (powersim/firmware.py: aes_firmware) leaking HW + Gaussian noise.
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))   # for `import powersim`
from powersim import SBOX   # noqa: E402,F401


# ---------------------------------------------------------------------------
# Exercise 2-1: difference-of-means traces for all 256 guesses of one key byte
# ---------------------------------------------------------------------------
def dom_traces(traces, pts, byte, bit=0):
    """traces (n, S) float, pts (n, 16) uint8.

    For every guess g of key byte `byte`, split the traces by the predicted selection bit
        D_g = bit `bit` of SBOX[pts[:, byte] ^ g]
    and return the (256, S) float array  mean(traces | D_g = 1) - mean(traces | D_g = 0)."""
    traces = np.asarray(traces, dtype=np.float64)
    n, S = traces.shape
    # TODO: build the (n, 256) matrix of selection bits (numpy broadcasting:
    #   pts[:, byte][:, None] ^ np.arange(256)[None, :]  gives every (trace, guess) pair),
    # then compute both partial means for all guesses at once.  Hint: if D is the 0/1 matrix,
    # D.T @ traces is the (256, S) sum of the traces whose bit is 1.
    return np.zeros((256, S))


# ---------------------------------------------------------------------------
# Exercise 2-2: recover the full key
# ---------------------------------------------------------------------------
def dpa_recover_key(traces, pts):
    """Return the 16-byte key (uint8 array, bytes, or list of ints)."""
    key = np.zeros(16, dtype=np.uint8)
    # TODO: for each byte, score every guess by the height of its difference-of-means peak
    # (max over samples of |D|) and keep the best.  Single-bit DPA can be fooled by "ghost
    # peaks" of wrong guesses; using several selection bits makes it much more robust.
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
    print(f"recovered {bytes(np.asarray(k, dtype=np.uint8).tolist()).hex()}\n"
          f"correct   {bytes(dev._key).hex()}  ({int((np.asarray(k) == dev._key).sum())}/16 bytes)")
