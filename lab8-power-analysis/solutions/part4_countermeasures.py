"""Part 4 -- Countermeasures and attacks on them.  (REFERENCE SOLUTION)

See README.md, Part 4.

    python3 src/part4_countermeasures.py tvla [--model hw|hd] [--impl masked|unmasked] [--n 2000]
    python3 src/part4_countermeasures.py demo   [--seed N]       # runs 4-2, 4-3, 4-4 on fresh devices
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))   # for `import powersim`
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))                      # for part3_cpa
from powersim import SBOX, SR_IDX   # noqa: E402
from part3_cpa import pearson, cpa_recover_key, hyp_hw_sbox   # noqa: E402


# ---------------------------------------------------------------------------
# Exercise 4-1: first-order Boolean masked AES-128
# ---------------------------------------------------------------------------
def masked_aes(cpu, p0, p1, rk):
    """Encrypt plaintext shares (p0 ^ p1 = plaintext), each (n, 16) uint8, with round keys
    rk (11, 16).  Return ciphertext shares (c0, c1), each (n, 16), with c0 ^ c1 = ciphertext.

    Masks (fresh per encryption, from cpu.random_bytes):
      m_in        S-box input mask: the whole state before every AddRoundKey+SubBytes
      m_out       S-box output mask (masked table T[x ^ m_in] = S[x] ^ m_out)
      mc[0..3]    one mask per row for MixColumns, so that no XOR of two state bytes cancels
                  the masks; MixColumns turns them into mc'[r] = MC(mc)[r]
    Every intermediate is (value) ^ (a non-zero GF(2)-linear combination of independent
    uniform masks), hence uniform and independent of the plaintext: first-order secure in the
    HW model.  (Under the HD model, consecutive values that share a mask leak; Discussion 4-2.)
    """
    n = cpu.n
    m_in = cpu.random_bytes(tag="mask.in")
    m_out = cpu.random_bytes(tag="mask.out")
    mc = cpu.random_bytes(4, tag="mask.mc")                                  # (n, 4)
    T = cpu.masked_table(SBOX, m_in, m_out)
    # mask-only precomputation (does not touch secret data)
    u = cpu.xor(mc[:, 0], mc[:, 1], tag="mask.mc01")
    u = cpu.xor(u, mc[:, 2], tag="mask.mc012")
    msum = cpu.xor(u, mc[:, 3], tag="mask.mcsum")                            # mc0^mc1^mc2^mc3
    mcp = np.empty((n, 4), dtype=np.uint8)                                   # MC(mc)
    for r in range(4):
        v = cpu.xor(mc[:, r], mc[:, (r + 1) % 4], tag=f"mask.mcv{r}")
        w = cpu.xtime(v, tag=f"mask.mcw{r}")
        o = cpu.xor(w, msum, tag=f"mask.mco{r}")
        mcp[:, r] = cpu.xor(o, mc[:, r], tag=f"mask.mcp{r}")
    to_rows = [cpu.xor(m_out, mc[:, r], tag=f"mask.out2row{r}") for r in range(4)]     # m_out ^ mc_r
    to_in = [cpu.xor(mcp[:, r], m_in, tag=f"mask.row2in{r}") for r in range(4)]        # mc'_r ^ m_in
    # ---- re-mask the input from (p1) to m_in:  st = p0 ^ (p1 ^ m_in) = pt ^ m_in
    st = np.empty((n, 16), dtype=np.uint8)
    for i in range(16):
        d = cpu.xor(p1[:, i], m_in, tag=f"in.remask[{i}]")                    # p1 ^ m_in (masks only)
        st[:, i] = cpu.xor(p0[:, i], d, tag=f"in.state[{i}]")                # pt ^ m_in
    for r in range(1, 11):
        k = rk[r - 1]
        # AddRoundKey + SubBytes: (s ^ m_in) ^ k -> T[.] = S(s ^ k) ^ m_out
        for i in range(16):
            a = cpu.load(st[:, i], tag=f"r{r}.load[{i}]")
            x = cpu.xor(a, k[i], tag=f"r{r}.ark[{i}]")
            y = cpu.lookup(T, x, tag=f"r{r}.sbox[{i}]")
            st[:, i] = cpu.store(y, tag=f"r{r}.store[{i}]")
        # ShiftRows (all bytes carry the same mask m_out, so moving them is harmless)
        st = st[:, SR_IDX]
        for i in range(16):
            if i % 4:
                cpu.mov(st[:, i], tag=f"r{r}.sr[{i}]", reg="r25")
        if r == 10:
            break
        # m_out -> row masks mc_r
        for i in range(16):
            st[:, i] = cpu.xor(st[:, i], to_rows[i % 4], tag=f"r{r}.torow[{i}]")
        # MixColumns on masked bytes (linear: masks become mc'_r)
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
        # mc'_r -> m_in for the next round
        for i in range(16):
            st[:, i] = cpu.xor(out[:, i], to_in[i % 4], tag=f"r{r}.toin[{i}]")
    # final AddRoundKey: ciphertext masked by m_out
    c0 = np.empty((n, 16), dtype=np.uint8)
    for i in range(16):
        c0[:, i] = cpu.xor(st[:, i], rk[10][i], tag=f"final.ark[{i}]")
        cpu.store(c0[:, i], tag=f"final.store[{i}]")
    c1 = np.repeat(m_out[:, None], 16, axis=1)
    return c0, c1


# ---------------------------------------------------------------------------
# Exercise 4-2 (optional): second-order CPA against MaskedSboxDevice
# ---------------------------------------------------------------------------
def second_order_attack(traces, pts, tags):
    """Combine the sample that leaks HW(m_out) with the samples that leak HW(S(p ^ k) ^ m_out)
    using the centered product, then run CPA on the combined 'trace'.  Return the key, or
    None if you skip this exercise."""
    T = np.asarray(traces, dtype=np.float64)
    T = T - T.mean(axis=0)
    pm = tags.index("mask.out")
    key = np.zeros(16, dtype=np.uint8)
    for b in range(16):
        cols = [tags.index(f"r1.sbox[{b}]"), tags.index(f"r1.store[{b}]")]
        comb = np.stack([T[:, pm] * T[:, c] for c in cols], axis=1)        # (n, 2)
        r = pearson(comb, hyp_hw_sbox(pts, b))                               # (256, 2)
        key[b] = int(np.argmax(np.abs(r).max(axis=1)))
    return key


# ---------------------------------------------------------------------------
# Exercise 4-3: random delays -> re-align by correlation, then CPA
# ---------------------------------------------------------------------------
LOOP_PATTERN = np.tile([4.0, 2.0, 6.0, 3.0], 16)      # BASE of load/xor/lookup/store, 16 bytes
LOOP_PATTERN = LOOP_PATTERN + 4.0                     # + average HW of a byte


def _best_offsets(traces, tpl):
    """For every trace, the offset o maximizing corr(trace[o:o+len(tpl)], tpl)."""
    L = len(tpl)
    W = np.lib.stride_tricks.sliding_window_view(traces, L, axis=1)       # (n, S-L+1, L)
    Wc = W - W.mean(axis=2, keepdims=True)
    t = tpl - tpl.mean()
    c = (Wc @ t) / (np.sqrt((Wc * Wc).sum(axis=2)) * np.sqrt((t * t).sum()) + 1e-12)
    return np.argmax(c, axis=1)


def align(traces, pre=4, length=72):
    """Return an (n, length) array in which sample j of every row is the same instruction.

    1) locate the round-1 S-box loop in every trace by correlating with the known instruction
       pattern (BASE levels from the KINDS table); 2) build a template from the roughly aligned
       traces and re-align against it (the template now also carries the trigger/ShiftRows
       context).  The output starts `pre` samples before the loop."""
    T = np.asarray(traces, dtype=np.float64)
    off = _best_offsets(T, LOOP_PATTERN)
    for _ in range(2):
        seg = np.stack([T[i, o:o + len(LOOP_PATTERN)] for i, o in enumerate(off)])
        tpl = seg.mean(axis=0)
        off = _best_offsets(T, tpl)
    n, S = T.shape
    out = np.empty((n, length))
    for i, o in enumerate(off):
        a = max(0, o - pre)
        row = T[i, a:a + length]
        out[i, :len(row)] = row
        out[i, len(row):] = row.mean() if len(row) else 0.0
    return out


def attack_delays(traces, pts):
    """Recover the key from traces protected by trigger jitter + random delays."""
    return cpa_recover_key(align(traces), pts, model="hw")


# ---------------------------------------------------------------------------
# Exercise 4-4: shuffling -> integrate over the loop slots
# ---------------------------------------------------------------------------
def attack_shuffled(traces, pts, tags):
    """The 16 S-box lookups of round 1 happen in a random order, but always at the slots tagged
    'r1.sbox[j]' / 'r1.store[j]'.  Summing those 32 samples gives a per-trace value that
    contains HW(S(p_b ^ k_b)) for EVERY byte b, whatever the order; correlate it with each
    byte's hypothesis.  (Costs a factor ~sqrt(16) in correlation instead of 16.)"""
    T = np.asarray(traces, dtype=np.float64)
    cols = [tags.index(f"r1.sbox[{j}]") for j in range(16)] + \
           [tags.index(f"r1.store[{j}]") for j in range(16)]
    s = T[:, cols].sum(axis=1, keepdims=True)
    key = np.zeros(16, dtype=np.uint8)
    for b in range(16):
        r = pearson(s, hyp_hw_sbox(pts, b))[:, 0]
        key[b] = int(np.argmax(r))                     # sign is known: more ones -> more power
    return key


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
