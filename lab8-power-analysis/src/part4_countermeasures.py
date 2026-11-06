"""Part 4 -- Countermeasures and attacks on them.

See README.md, Part 4.

    python3 src/part4_countermeasures.py tvla [--model hw|hd] [--impl masked|unmasked] [--n 2000]
    python3 src/part4_countermeasures.py demo   [--seed N]       # runs 4-2, 4-3, 4-4 on fresh devices
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))   # for `import powersim`
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))                      # for part3_cpa
from powersim import SBOX, HW8, SR_IDX   # noqa: E402,F401
from part3_cpa import pearson, cpa_recover_key, hyp_hw_sbox   # noqa: E402,F401


# ---------------------------------------------------------------------------
# Exercise 4-1: first-order Boolean masked AES-128
# ---------------------------------------------------------------------------
def masked_aes(cpu, p0, p1, rk):
    """Encrypt plaintext shares (p0 ^ p1 = plaintext), each (n, 16) uint8, with round keys
    rk (11, 16).  Return ciphertext shares (c0, c1), each (n, 16), with c0 ^ c1 = ciphertext.

    Rules: every operation on state data goes through cpu.* (load/xor/xtime/lookup/store/mov),
    and no instruction may ever produce an unmasked value -- not even the plaintext or the
    ciphertext.  Fresh randomness: cpu.random_bytes(); per-trace masked S-box:
    cpu.masked_table(SBOX, m_in, m_out).

    The starter below is the unprotected firmware (powersim/firmware.py) rewritten in this
    interface: it is functionally correct and leaks everywhere.  Run
        python3 src/part4_countermeasures.py tvla
    to see where, then fix it step by step (TODO markers)."""
    n = cpu.n
    # TODO: draw the masks you need (S-box input/output masks; for MixColumns one mask per row)
    #       and build the masked table.

    # TODO: this UNMASKS the plaintext (p0 ^ p1 = pt).  Re-mask instead: combine p1 with your
    #       mask first (masks only), then XOR the result into p0.
    st = np.empty((n, 16), dtype=np.uint8)
    for i in range(16):
        st[:, i] = cpu.xor(p0[:, i], p1[:, i], tag=f"in.state[{i}]")
    for r in range(1, 11):
        k = rk[r - 1]
        # AddRoundKey + SubBytes
        for i in range(16):
            a = cpu.load(st[:, i], tag=f"r{r}.load[{i}]")
            x = cpu.xor(a, k[i], tag=f"r{r}.ark[{i}]")
            y = cpu.lookup(SBOX, x, tag=f"r{r}.sbox[{i}]")          # TODO: masked table
            st[:, i] = cpu.store(y, tag=f"r{r}.store[{i}]")
        # ShiftRows
        st = st[:, SR_IDX]
        for i in range(16):
            if i % 4:
                cpu.mov(st[:, i], tag=f"r{r}.sr[{i}]", reg="r25")
        if r == 10:
            break
        # TODO: before MixColumns, change the masks so that XORing two bytes of a column does
        #       not cancel them (think about what a0 ^ a1 is if both carry the same mask).
        out = np.empty_like(st)
        for c in range(4):
            a = [st[:, 4 * c + b] for b in range(4)]
            u = cpu.xor(a[0], a[1], tag=f"r{r}.mc{c}.t01", reg="r18")
            u = cpu.xor(u, a[2], tag=f"r{r}.mc{c}.t012", reg="r19")
            t = cpu.xor(u, a[3], tag=f"r{r}.mc{c}.t", reg="r20")
            for b in range(4):
                v = cpu.xor(a[b], a[(b + 1) % 4], tag=f"r{r}.mc{c}.v{b}", reg="r21")
                w = cpu.xtime(v, tag=f"r{r}.mc{c}.w{b}", reg="r22")
                o = cpu.xor(w, t, tag=f"r{r}.mc{c}.wt{b}", reg="r23")
                out[:, 4 * c + b] = cpu.xor(o, a[b], tag=f"r{r}.mc{c}.o{b}", reg="r24")
        st = out
        # TODO: MixColumns is linear, so the output masks are MixColumns(input masks)
        #       (powersim.mix_column_masks, or compute them with cpu.* instructions).
        #       Re-mask to whatever the next round's masked table expects.
    c0 = np.empty((n, 16), dtype=np.uint8)
    for i in range(16):
        c0[:, i] = cpu.xor(st[:, i], rk[10][i], tag=f"final.ark[{i}]")
        cpu.store(c0[:, i], tag=f"final.store[{i}]")
    # TODO: return the masked ciphertext and its mask as the two shares
    return c0, np.zeros_like(c0)


# ---------------------------------------------------------------------------
# Exercise 4-2 (optional): second-order CPA against MaskedSboxDevice
# ---------------------------------------------------------------------------
def second_order_attack(traces, pts, tags):
    """traces from MaskedSboxDevice (see masked_sbox_firmware): find the sample that leaks the
    output mask and the samples that leak the masked S-box output (tags tell you where),
    combine them with the centered product, and run CPA on the combination.
    Return the 16-byte key, or None to skip this optional exercise."""
    # TODO (optional)
    return None


# ---------------------------------------------------------------------------
# Exercise 4-3: random delays -> re-align by correlation, then CPA
# ---------------------------------------------------------------------------
def align(traces):
    """traces (n, S) from a device with trigger jitter (0..60) and random delays (0..40 dummy
    instructions before every round).  Return an (n, L) array in which column j is the same
    instruction in every row, covering at least the round-1 S-box loop (64 samples)."""
    T = np.asarray(traces, dtype=np.float64)
    # TODO: pick a template: e.g. the instruction pattern of the S-box loop (BASE levels of
    # load/xor/lookup/store from the KINDS table in powersim/leakage.py), or a stretch of one
    # trace that you located by eye.  Slide it over every trace, keep the offset with the
    # highest correlation, cut a fixed-length window there.  Refine: average the cut windows
    # into a cleaner template and repeat.
    return T


def attack_delays(traces, pts):
    """Recover the key from the misaligned traces."""
    return cpa_recover_key(align(traces), pts, model="hw")


# ---------------------------------------------------------------------------
# Exercise 4-4: shuffling
# ---------------------------------------------------------------------------
def attack_shuffled(traces, pts, tags):
    """The device processes the 16 bytes of every AddRoundKey+SubBytes loop (and the ShiftRows
    moves) in a fresh random order; tags name the loop *slot*: 'r1.sbox[j]' is the j-th lookup
    of round 1, whichever byte it happened to be.  Return the 16-byte key."""
    # TODO: plain CPA (below) only sees each byte at a given slot 1/16 of the time.  Combine
    # the slots instead (Discussion 4-4 asks why that helps, and by how much).
    return cpa_recover_key(traces, pts, model="hw")


# ---------------------------------------------------------------------------
# harness (not graded): TVLA on your masked implementation, demos
# ---------------------------------------------------------------------------
def unmasked_reference(cpu, p0, p1, rk):
    """The unprotected firmware, with the same interface as masked_aes (for comparison)."""
    from powersim import firmware
    return firmware.aes_firmware(cpu, p0 ^ p1, rk), np.zeros_like(p0)


def run_tvla(impl, model="hw", n=2000, noise=0.5, seed=None, quiet=False):
    """Fixed-vs-random Welch t-test (two independent repetitions).  Returns (t1, t2, tags)."""
    from powersim import Recorder, expand_key, aes_encrypt
    from powersim.stats import welch_t
    rng = np.random.default_rng(seed)
    key = rng.integers(0, 256, 16, dtype=np.uint8)
    rk = expand_key(key)
    fixed = rng.integers(0, 256, 16, dtype=np.uint8)
    ts, tags = [], None
    for rep in range(2):
        groups = []
        for g in ("fixed", "random"):
            pt = np.tile(fixed, (n, 1)) if g == "fixed" else rng.integers(0, 256, (n, 16), dtype=np.uint8)
            m0 = rng.integers(0, 256, (n, 16), dtype=np.uint8)
            cpu = Recorder(n, model, rng=rng)
            c0, c1 = impl(cpu, pt ^ m0, m0, rk)
            if not ((c0 ^ c1) == aes_encrypt(pt, key)).all():
                raise AssertionError("ciphertext shares do not recombine to AES(pt)")
            groups.append(cpu.render(noise=noise))
            tags = cpu.tags
        if groups[0].shape != groups[1].shape:
            raise AssertionError("fixed and random runs have different lengths")
        ts.append(welch_t(groups[0], groups[1]))
    if not quiet:
        leak = np.nonzero((np.abs(ts[0]) > 4.5) & (np.abs(ts[1]) > 4.5))[0]
        print(f"TVLA ({model}, n={n} per group, noise={noise}): {len(tags)} samples, "
              f"max|t| = {np.abs(ts[0]).max():.1f} / {np.abs(ts[1]).max():.1f}, "
              f"{len(leak)} samples above 4.5 in both runs")
        for s in leak[:25]:
            print(f"    sample {s:5d}  t = {ts[0][s]:+8.1f} {ts[1][s]:+8.1f}   {tags[s]}")
        if len(leak) > 25:
            print(f"    ... and {len(leak) - 25} more")
    return ts[0], ts[1], tags


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=("tvla", "demo"))
    ap.add_argument("--model", choices=("hw", "hd"), default="hw")
    ap.add_argument("--impl", choices=("masked", "unmasked"), default="masked")
    ap.add_argument("--n", type=int, default=2000)
    ap.add_argument("--noise", type=float, default=0.5)
    ap.add_argument("--seed", type=int, default=None)
    a = ap.parse_args()
    if a.what == "tvla":
        run_tvla(masked_aes if a.impl == "masked" else unmasked_reference, a.model, a.n, a.noise, a.seed)
    else:
        from powersim import AESDevice, MaskedSboxDevice
        s = a.seed
        dev = MaskedSboxDevice(seed=s, noise=1.0)
        ts = dev.capture(4000)
        k1 = cpa_recover_key(ts.traces, ts.pts)
        k2 = second_order_attack(ts.traces, ts.pts, ts.tags)
        print(f"4-2 masked device: 1st-order CPA {int((k1 == dev._key).sum())}/16, "
              f"2nd-order {'skipped' if k2 is None else str(int((np.asarray(k2) == dev._key).sum())) + '/16'}")
        dev = AESDevice(seed=None if s is None else s + 1, noise=1.5, jitter=60, round_delay=40, window=(0, 300))
        ts = dev.capture(1000)
        k1 = cpa_recover_key(ts.traces, ts.pts)
        k2 = attack_delays(ts.traces, ts.pts)
        print(f"4-3 delays: plain CPA {int((k1 == dev._key).sum())}/16, after alignment "
              f"{int((np.asarray(k2) == dev._key).sum())}/16")
        dev = AESDevice(seed=None if s is None else s + 2, noise=3.0, shuffle=True)
        ts = dev.capture(3000)
        k1 = cpa_recover_key(ts.traces, ts.pts)
        k2 = attack_shuffled(ts.traces, ts.pts, ts.tags)
        print(f"4-4 shuffling: plain CPA {int((k1 == dev._key).sum())}/16, your attack "
              f"{int((np.asarray(k2) == dev._key).sum())}/16")
