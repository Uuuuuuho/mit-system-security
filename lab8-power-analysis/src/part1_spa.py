"""Part 1 -- Simple power analysis of square-and-multiply.

See README.md, Part 1.  Run this file directly to try your code on a fresh device:

    python3 src/part1_spa.py [--seed N]
    python3 -m powersim show modexp --bits 16 --noise 0.3        # look at a short exponentiation first!

Layout of one exponentiation (powersim/firmware.py: modexp_sqm), W = nwords = 8 by default:
    16 x nop | per exponent bit, MSB first: 3 x loop, W x sqr, [W x mulrd, W x mul if bit = 1] | 16 x nop
Sample = BASE + GAIN * HW(32-bit word) + noise:
    nop 0 | loop 1.0 | sqr 6.0 + 0.25 HW | mulrd 4.5 + 0.25 HW | mul 6.5 + 0.25 HW
The exponent has exactly nbits bits and its top bit is always 1.
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))   # for `import powersim`

TRIGGER = 16


# ---------------------------------------------------------------------------
# Exercise 1-1: read the exponent from one clean, aligned trace
# ---------------------------------------------------------------------------
def recover_exponent(trace, nbits, nwords):
    """trace: 1-D float array of one exponentiation (no jitter, noise sigma = 1.0): the 16 trigger
    nops are samples 0..15.  Return the secret exponent as a Python int."""
    trace = np.asarray(trace, dtype=np.float64)
    # TODO: walk through the trace and decide, for each of the nbits iterations, whether a
    # multiplication followed the squaring.  Two workable approaches:
    #   * segment: threshold the trace into "busy" (sqr/mul) and "quiet" (loop/nop) runs and
    #     look at the length of each busy run (W samples -> bit 0, 3W samples -> bit 1);
    #   * walk the known layout: position p = 16; repeat nbits times: skip loop + sqr, then
    #     look at the next 3 samples -- loop level (~1) or mulrd level (~8.5)?
    # Average a few samples per decision: single samples are noisy.
    return 0


# ---------------------------------------------------------------------------
# Exercise 1-2: many noisy traces with trigger jitter
# ---------------------------------------------------------------------------
def recover_exponent_multi(traces, nbits, nwords):
    """traces: (n, S) float array: same exponent, random bases, noise sigma = 8, and a random
    trigger offset of 0..300 samples per trace (the extra samples are unrelated code).
    Return the secret exponent as a Python int."""
    traces = np.asarray(traces, dtype=np.float64)
    # TODO:
    #   1. estimate the offset of every trace relative to one reference trace with
    #      cross-correlation (np.correlate on mean-removed traces, or FFTs);
    #   2. shift and average the traces (noise drops by sqrt(n));
    #   3. optionally re-align every trace against the average and average again;
    #   4. find where the exponentiation starts (the 16 nops sit at level 0, the code around
    #      them does not) and decode the average like in Exercise 1-1.
    return 0


# ---------------------------------------------------------------------------
# Exercise 1-3: a regular exponentiation (defense)
# ---------------------------------------------------------------------------
def ladder(cpu, bases, e, mod, nbits, nwords):
    """Return [pow(b, e, mod) for b in bases] using only cpu.loop(), cpu.modmul() and
    cpu.modsqr(), such that the sequence of instructions does NOT depend on e.

    The starter below is plain square-and-multiply: correct, but its instruction sequence spells
    out the exponent.  TODO: turn it into a Montgomery ladder (keep two registers R0, R1 with
    the invariant R1 = R0 * base; one modmul and one modsqr per bit)."""
    R = [1] * cpu.n
    for i in range(nbits - 1, -1, -1):
        cpu.loop()
        R = cpu.modsqr(R, mod, nwords)
        if (e >> i) & 1:
            R = cpu.modmul(R, bases, mod, nwords)
    return R


# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import argparse
    from powersim import ModExpDevice
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=None)
    a = ap.parse_args()
    dev = ModExpDevice(seed=a.seed, noise=1.0)
    ts = dev.capture(1)
    e = recover_exponent(ts.traces[0], dev.nbits, dev.nwords)
    print(f"1-1: recovered {e:#x}\n     correct   {dev._e:#x}  ->  {'OK' if e == dev._e else 'WRONG'}")
    dev = ModExpDevice(seed=None if a.seed is None else a.seed + 1, noise=8.0, jitter=300)
    ts = dev.capture(25)
    e = recover_exponent_multi(ts.traces, dev.nbits, dev.nwords)
    print(f"1-2: recovered {e:#x}\n     correct   {dev._e:#x}  ->  {'OK' if e == dev._e else 'WRONG'}")
