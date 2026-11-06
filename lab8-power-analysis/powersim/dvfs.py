"""A toy DVFS governor: how a power side channel becomes a timing side channel.

Modern CPUs keep package power under a limit by lowering the clock frequency
when (filtered) power is high.  If power depends on the data being processed
(Hamming weight!), then frequency, and therefore *wall-clock time*, depends on
the data too, even for constant-cycle code.  This is the mechanism behind
Hertzbleed (Wang et al., USENIX Security 2022).  This module models it with a
first-order low-pass filter and a two-level frequency controller.  It is not
graded; Discussion 4-5 uses it.

    python3 -m powersim dvfs           # demo: runtime vs Hamming weight of a secret
"""

import numpy as np

from .leakage import Recorder


def dvfs_runtime(power, f_hi=4.0, f_lo=2.0, budget=26.0, tau=400.0):
    """power: (n, S) noise-free energy per cycle.  Returns wall-clock runtime per trace (n,),
    in units of 1/frequency (ns if frequencies are in GHz).

    Power = energy per cycle x frequency.  The governor tracks an exponential moving average
    `avg` of the energy per cycle (time constant tau cycles) and picks the highest frequency
    that keeps power within the budget:  f = clip(budget / avg, f_lo, f_hi).  Below the knee
    (budget / avg > f_hi) the clock is pinned at f_hi and runtime is data independent; above
    it, runtime grows linearly with the average switching activity, i.e. with Hamming weight."""
    power = np.asarray(power, dtype=np.float64)
    n, S = power.shape
    avg = power[:, :min(S, 64)].mean(axis=1)
    t = np.zeros(n)
    alpha = 1.0 / tau
    for s in range(S):
        avg += alpha * (power[:, s] - avg)
        t += 1.0 / np.clip(budget / np.maximum(avg, 1e-9), f_lo, f_hi)
    return t


def checksum_workload(cpu, secret, passes=64):
    """Constant-time code: XOR-accumulate a secret buffer `passes` times (no branches, no
    secret-dependent addresses).  secret: (n, k) uint8."""
    acc = np.zeros(cpu.n, dtype=np.uint8)
    for _ in range(passes):
        for j in range(secret.shape[1]):
            v = cpu.load(secret[:, j])
            acc = cpu.xor(acc, v)
    return acc


def demo(seed=None, n=200, timer_noise=0.5):
    rng = np.random.default_rng(seed)
    print("toy DVFS: constant-time XOR checksum over a 16-byte secret, 64 passes (2048 instructions)")
    print(f"timer noise sigma = {timer_noise} time units per measurement\n")
    print(" HW(secret)   mean runtime   std     (all runs execute the same number of cycles)")
    res = {}
    for hwt in (16, 48, 64, 80, 112):
        sec = np.zeros((n, 16), dtype=np.uint8)
        for i in range(n):                               # random secret with exactly hwt one-bits
            bits = np.zeros(128, dtype=np.uint8)
            bits[rng.choice(128, hwt, replace=False)] = 1
            sec[i] = np.packbits(bits)
        cpu = Recorder(n, "hw", rng=rng)
        checksum_workload(cpu, sec)
        p = cpu.render(noise=0.0)
        t = dvfs_runtime(p) + rng.normal(0, timer_noise, n)
        res[hwt] = t
        print(f"   {hwt:4d}      {t.mean():10.2f}   {t.std():6.2f}")
    from .stats import welch_t
    tt = welch_t(res[16][:, None], res[112][:, None])[0]
    print(f"\nWelch t (HW 16 vs HW 112, {n} runs each): {tt:.1f}  -> the runtime reveals the Hamming weight")
