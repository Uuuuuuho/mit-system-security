"""Part 1 -- Simple power analysis of square-and-multiply.  (REFERENCE SOLUTION)

See README.md, Part 1.  Run this file directly to try your code on a fresh device:

    python3 src/part1_spa.py [--seed N]

Layout of one exponentiation (firmware.modexp_sqm), W = nwords = 8 by default:
    16 x nop | per exponent bit, MSB first: 3 x loop, W x sqr, [W x mulrd, W x mul if bit = 1] | 16 x nop
Kinds (BASE + GAIN * HW(32-bit word)): loop 1.0 | sqr 6.0 + 0.25 HW | mulrd 4.5 + 0.25 HW | mul 6.5 + 0.25 HW
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))   # for `import powersim`

TRIGGER = 16


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
def find_start(trace, nwords=8):
    """Index of the first instruction after the 16-nop trigger region.

    Least-squares match of the known beginning of every run: 16 nops (0), loop (1.0), the first
    square (R = 1*1, so HW ~ 1: level ~6.0) and -- because the exponent's top bit is always 1 --
    mulrd (~8.5) and mul (~10.5).  The trailing 16 nops look similar, so search the first half."""
    W = nwords
    tpl = np.concatenate([np.zeros(TRIGGER), np.full(3, 1.0), np.full(W, 6.0),
                          np.full(W, 8.5), np.full(W, 10.5)])
    n = max(1, min(len(trace) - len(tpl) + 1, len(trace) // 2))
    win = np.lib.stride_tricks.sliding_window_view(trace, len(tpl))[:n]
    ssd = ((win - tpl[None, :]) ** 2).sum(axis=1)
    return int(np.argmin(ssd)) + TRIGGER


def decode(trace, nbits, nwords, start=None):
    """Walk the known instruction layout: after loop + sqr, three low samples mean 'next loop'
    (bit 0); high samples mean 'mulrd' (bit 1)."""
    p = find_start(trace, nwords) if start is None else start
    thr = 0.5 * (1.0 + 4.5 + 0.25 * 16)                  # between loop (1.0) and mulrd (~8.5)
    e = 0
    for _ in range(nbits):
        p += 3 + nwords                                  # loop + square
        bit = 1 if np.mean(trace[p:p + 3]) > thr else 0
        e = (e << 1) | bit
        if bit:
            p += 2 * nwords                              # mulrd + mul
    return e


# ---------------------------------------------------------------------------
# Exercise 1-1: read the exponent from one clean, aligned trace
# ---------------------------------------------------------------------------
def recover_exponent(trace, nbits, nwords):
    """trace: 1-D float array (one exponentiation, no jitter, low noise).  Return e as an int."""
    return decode(trace, nbits, nwords, start=TRIGGER)


# ---------------------------------------------------------------------------
# Exercise 1-2: many noisy traces with trigger jitter
# ---------------------------------------------------------------------------
def align_lags(traces, ref):
    """Lag of every trace relative to ref by full cross-correlation:  x[t + lag] ~ ref[t]."""
    S = traces.shape[1]
    r = ref - ref.mean()
    nfft = 1 << int(np.ceil(np.log2(2 * S)))
    R = np.conj(np.fft.rfft(r, nfft))
    lags = []
    for x in traces:
        c = np.fft.irfft(np.fft.rfft(x - x.mean(), nfft) * R, nfft)
        k = int(np.argmax(c))
        lags.append(k if k < nfft // 2 else k - nfft)
    return np.array(lags)


def shift_average(traces, lags):
    S = traces.shape[1]
    J = int(np.max(np.abs(lags))) + 1
    acc = np.zeros(S + 2 * J)
    cnt = np.zeros(S + 2 * J)
    for x, lag in zip(traces, lags):
        acc[J - lag:J - lag + S] += x
        cnt[J - lag:J - lag + S] += 1
    avg = acc / np.maximum(cnt, 1)
    avg[cnt < len(traces) // 2] = np.median(traces)      # poorly covered edges: neutral filler
    return avg


def recover_exponent_multi(traces, nbits, nwords):
    """traces: (n, S) float array, same exponent, random bases, heavy noise, random trigger
    offsets.  Return e as an int."""
    traces = np.asarray(traces, dtype=np.float64)
    lags = align_lags(traces, traces[0])
    avg = shift_average(traces, lags)
    for _ in range(2):                                   # re-align to the (much cleaner) average
        lags = align_lags(traces, avg)
        lags -= lags[0]
        avg = shift_average(traces, lags)
    return decode(avg, nbits, nwords)


# ---------------------------------------------------------------------------
# Exercise 1-3: a regular exponentiation (defense)
# ---------------------------------------------------------------------------
def ladder(cpu, bases, e, mod, nbits, nwords):
    """Montgomery ladder: the same instruction sequence (loop, modmul, modsqr) for every bit.
    Return [pow(b, e, mod) for b in bases]."""
    R0 = [1] * cpu.n
    R1 = list(bases)
    for i in range(nbits - 1, -1, -1):
        cpu.loop()
        if (e >> i) & 1:
            R0 = cpu.modmul(R0, R1, mod, nwords, reg="R0")
            R1 = cpu.modsqr(R1, mod, nwords, reg="R1")
        else:
            R1 = cpu.modmul(R0, R1, mod, nwords, reg="R1")
            R0 = cpu.modsqr(R0, mod, nwords, reg="R0")
    return R0


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
