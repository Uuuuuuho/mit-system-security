"""Victim firmware, written against the Recorder ("cpu") instruction API.

Read this file like a disassembly listing: every cpu.* call is one instruction
and therefore one sample in the trace (see leakage.py for the sample model).

    aes_firmware          unprotected byte-oriented AES-128 (Parts 2-4); also the
                          template for your masked implementation (Exercise 4-1)
    masked_sbox_firmware  first round of a first-order masked AES (Exercise 4-2)
    modexp_sqm            left-to-right square-and-multiply (Part 1)

Instruction layout of aes_firmware (no delays, no jitter), sample index:
    0..7       8 x nop                                    "trigger"
    then for each round r = 1..10:
      64       16 x [load s_i, xor k_i, lookup SBOX, store]  "r{r}.load[j]" ... "r{r}.store[j]"
      12       ShiftRows moves of rows 1..3 (i % 4 != 0)     "r{r}.sr[i]"
      76       MixColumns (rounds 1..9 only), 19 per column  "r{r}.mc{c}.*"
    32         final AddRoundKey: 16 x [xor k10_i, store]     "final.ark[i]", "final.store[i]"
Round 1 therefore occupies samples 8..159 and its S-box loop samples 8..71:
sample 8 + 4*i + {0,1,2,3} = {load, xor, lookup, store} of byte i.
"""

import numpy as np

from .aes import SBOX, SR_IDX

TRIGGER_NOPS = 8
SR_MOVES = np.array([i for i in range(16) if i % 4], dtype=np.intp)   # state bytes moved by ShiftRows


def aes_firmware(cpu, pts, rk, shuffle=False, round_delay=0):
    """Encrypt pts (n, 16) with round keys rk (11, 16).  Returns ciphertexts (n, 16).

    shuffle      True: process the 16 bytes of each AddRoundKey+SubBytes loop, and the 12
                 ShiftRows moves, in a fresh random order per trace (tags then name the *slot* j,
                 not the byte).  "sbox": shuffle only the AddRoundKey+SubBytes loops.
    round_delay  insert 0..round_delay random dummy instructions before every round
    """
    n = cpu.n
    rows = cpu.rows
    st = np.array(pts, dtype=np.uint8, copy=True)
    cpu.nop(TRIGGER_NOPS, tag="trigger")
    for r in range(1, 11):
        if round_delay:
            cpu.delay(round_delay)
        k = rk[r - 1]
        # ---- AddRoundKey(k_{r-1}) fused with SubBytes, one byte at a time
        if shuffle:
            order = np.argsort(cpu.rng.random((n, 16)), axis=1)
        for j in range(16):
            i = order[:, j] if shuffle else j
            s = st[rows, i] if shuffle else st[:, i]
            a = cpu.load(s, tag=f"r{r}.load[{j}]")
            x = cpu.xor(a, k[i], tag=f"r{r}.ark[{j}]")
            y = cpu.lookup(SBOX, x, tag=f"r{r}.sbox[{j}]")
            cpu.store(y, tag=f"r{r}.store[{j}]")
            if shuffle:
                st[rows, i] = y
            else:
                st[:, i] = y
        # ---- ShiftRows: row 0 stays, rows 1..3 rotate (12 byte moves)
        st = st[:, SR_IDX]
        if shuffle and shuffle != "sbox":
            perm = SR_MOVES[np.argsort(cpu.rng.random((n, 12)), axis=1)]      # (n, 12) byte indices
            for j in range(12):
                cpu.mov(st[rows, perm[:, j]], tag=f"r{r}.sr[{j}]", reg="r25")
        else:
            for i in SR_MOVES:
                cpu.mov(st[:, i], tag=f"r{r}.sr[{i}]", reg="r25")
        # ---- MixColumns
        if r < 10:
            st = mix_columns_fw(cpu, st, tag=f"r{r}")
    # ---- final AddRoundKey(k10)
    for i in range(16):
        c = cpu.xor(st[:, i], rk[10][i], tag=f"final.ark[{i}]")
        cpu.store(c, tag=f"final.store[{i}]")
        st[:, i] = c
    return st


def mix_columns_fw(cpu, st, tag):
    """MixColumns on (n, 16) state, 19 instructions per column:
        t = a0 ^ a1 ^ a2 ^ a3                          (3 xors)
        out_r = a_r ^ t ^ xtime(a_r ^ a_{r+1})         (xor, xtime, xor, xor per row)
    Each kind of intermediate has its own register (r18..r24), which matters for the HD model:
    an in-place update  r ^= a  would flip exactly HW(a) bits."""
    out = np.empty_like(st)
    for c in range(4):
        a = [st[:, 4 * c + r] for r in range(4)]
        u = cpu.xor(a[0], a[1], tag=f"{tag}.mc{c}.t01", reg="r18")
        u = cpu.xor(u, a[2], tag=f"{tag}.mc{c}.t012", reg="r19")
        t = cpu.xor(u, a[3], tag=f"{tag}.mc{c}.t", reg="r20")
        for r in range(4):
            v = cpu.xor(a[r], a[(r + 1) % 4], tag=f"{tag}.mc{c}.v{r}", reg="r21")
            w = cpu.xtime(v, tag=f"{tag}.mc{c}.w{r}", reg="r22")
            o = cpu.xor(w, t, tag=f"{tag}.mc{c}.wt{r}", reg="r23")
            out[:, 4 * c + r] = cpu.xor(o, a[r], tag=f"{tag}.mc{c}.o{r}", reg="r24")
    return out


def masked_sbox_firmware(cpu, pts, rk):
    """First round (AddRoundKey + SubBytes) of a first-order Boolean-masked AES.

    Two fresh random masks per encryption: m_in masks the S-box input, m_out its output.
    The capture window ends after this loop, so the function returns the masked S-box
    outputs (n, 16) together with m_out (for the self-test only; an attacker never sees them).

    Layout: 8 nop | rng m_in | rng m_out | 16 table stores | 16 x [load p_i, xor m_in,
            xor k_i, lookup T, store]  -> sample index of "r1.sbox[i]" = 26 + 5*i + 3.
    """
    cpu.nop(TRIGGER_NOPS, tag="trigger")
    m_in = cpu.random_bytes(tag="mask.in")
    m_out = cpu.random_bytes(tag="mask.out")
    T = cpu.masked_table(SBOX, m_in, m_out)
    k = rk[0]
    out = np.empty((cpu.n, 16), dtype=np.uint8)
    for i in range(16):
        p = cpu.load(pts[:, i], tag=f"r1.load[{i}]")
        a = cpu.xor(p, m_in, tag=f"r1.mask[{i}]")             # p_i ^ m_in
        x = cpu.xor(a, k[i], tag=f"r1.ark[{i}]")               # (p_i ^ k_i) ^ m_in
        y = cpu.lookup(T, x, tag=f"r1.sbox[{i}]")              # S(p_i ^ k_i) ^ m_out
        out[:, i] = cpu.store(y, tag=f"r1.store[{i}]")
    return out, m_out


def modexp_sqm(cpu, bases, e, mod, nbits, nwords):
    """Left-to-right binary exponentiation  R = base^e mod mod  for a list of n bases.

        R = 1
        for i = nbits-1 .. 0:
            (loop bookkeeping)
            R = R*R mod m
            if bit i of e is 1:  R = R*base mod m
    """
    cpu.nop(16, tag="trigger")
    R = [1] * cpu.n
    for i in range(nbits - 1, -1, -1):
        cpu.loop()
        R = cpu.modsqr(R, mod, nwords)
        if (e >> i) & 1:
            R = cpu.modmul(R, bases, mod, nwords)
    cpu.nop(16, tag="done")
    return R
