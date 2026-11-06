"""Leakage models and the trace recorder ("the oscilloscope").

Firmware in this lab is ordinary Python that runs n encryptions *in parallel*
(one per row of a numpy array) and routes every data-processing step through a
Recorder object `cpu`:

    x = cpu.xor(a, k, tag="r1.ark[0]")      # computes a ^ k AND records one sample

Each call is one "instruction" and produces one power sample per trace:

    sample = BASE[kind] + GAIN[kind] * L + noise

    L = HW(v)            Hamming-weight model  (model="hw"): the value v that the
                         instruction produces is driven onto a precharged bus.
    L = HD(prev, v)      Hamming-distance model (model="hd"): the instruction
                         overwrites register `reg`; power ~ number of flipped bits.
                         A register's contents before its first write are random.

BASE depends only on the instruction kind (that is what makes simple power
analysis possible); GAIN * L is the data-dependent part that DPA/CPA exploit;
noise is i.i.d. Gaussian, added when the traces are rendered.  Only the RESULT
of an instruction leaks: operands are assumed to be in registers already.

render() also implements misalignment: trigger jitter (a random number of
unrelated instructions before the trigger point) and the random delays that
firmware inserts with cpu.delay().
"""

import numpy as np

from .aes import XTIME

HW8 = np.array([bin(i).count("1") for i in range(256)], dtype=np.uint8)

# kind: (BASE, GAIN).  BASE is in the same unit as one bit of Hamming weight.
KINDS = {
    "nop":    (0.0, 0.0),    # idle cycles around the trigger
    "load":   (4.0, 1.0),    # 8-bit load from RAM
    "xor":    (2.0, 1.0),
    "lookup": (6.0, 1.0),    # table lookup (flash read, e.g. AVR lpm)
    "store":  (3.0, 1.0),
    "mov":    (2.5, 1.0),
    "xtime":  (3.5, 1.0),
    "rng":    (5.0, 1.0),    # reading a fresh random byte from the TRNG
    "dummy":  (3.0, 1.0),    # random dummy instruction (random delays / unrelated code)
    "loop":   (1.0, 0.0),    # loop bookkeeping between bignum operations
    "sqr":    (6.0, 0.25),   # one 32-bit word written by a big-number squaring
    "mulrd":  (4.5, 0.25),   # one 32-bit operand word fetched by a big-number multiply
    "mul":    (6.5, 0.25),   # one 32-bit word written by a big-number multiply
}

# Background ("other code") samples are drawn from these base levels + HW(random byte).
_BACKGROUND_LEVELS = np.array([2.0, 2.5, 3.0, 3.5, 4.0, 6.0], dtype=np.float32)


def hw(x):
    """Hamming weight of every element of an unsigned-integer array (up to 64 bits)."""
    x = np.asarray(x)
    if x.dtype == np.uint8:
        return HW8[x]
    if x.dtype.kind not in "ui":
        raise TypeError(f"hw() needs an integer array, got {x.dtype}")
    if x.dtype.kind == "i" and x.size and x.min() < 0:
        raise ValueError("hw() of a negative number")
    x = x.astype(np.uint64)
    out = np.zeros(x.shape, dtype=np.int16)
    for s in range(0, 8 * x.dtype.itemsize, 8):
        out += HW8[(x >> np.uint64(s)) & np.uint64(0xFF)]
    return out


def to_words(values, nwords):
    """List of n Python ints -> (n, nwords) uint32 array, least significant word first."""
    out = np.zeros((len(values), nwords), dtype=np.uint32)
    for i, v in enumerate(values):
        for j in range(nwords):
            out[i, j] = (v >> (32 * j)) & 0xFFFFFFFF
    return out


def _u8(v):
    return np.asarray(v).astype(np.uint8, copy=False)


def background(n, length, rng):
    """Samples of unrelated code (no noise yet): random base level + HW(random byte)."""
    lv = _BACKGROUND_LEVELS[rng.integers(0, len(_BACKGROUND_LEVELS), size=(n, length))]
    return lv + HW8[rng.integers(0, 256, size=(n, length))].astype(np.float32)


class Recorder:
    """Runs vectorized firmware on n inputs and records one leakage value per instruction.

    Attributes after a run: kinds (list of str), tags (list of str or None), len(cpu) = #instructions.
    """

    def __init__(self, n, model="hw", rng=None):
        if model not in ("hw", "hd"):
            raise ValueError("model must be 'hw' or 'hd'")
        self.n = int(n)
        self.model = model
        self.rng = rng if rng is not None else np.random.default_rng()
        self.rows = np.arange(self.n)
        self.kinds = []
        self.tags = []
        self._L = []          # one (n,) float32 array per instruction
        self._regs = {}       # register name -> last value (HD model)
        self._delays = []     # (instruction index, per-trace delay, max delay)

    def __len__(self):
        return len(self.kinds)

    # ------------------------------------------------------------------ core
    def leak(self, value, kind, reg="r24", tag=None):
        """Record one instruction of `kind` producing `value` (shape (n,) or scalar) and return value.
        A 2-D value (n, k) records k instructions, one per column."""
        if kind not in KINDS:
            raise ValueError(f"unknown instruction kind {kind!r}")
        v = np.asarray(value)
        if v.ndim == 0:
            v = np.full(self.n, v, dtype=v.dtype if v.dtype.kind in "ui" else np.int64)
        if v.shape[0] != self.n:
            raise ValueError(f"leak(): expected {self.n} values (one per trace), got shape {v.shape}")
        if v.ndim == 2:
            for j in range(v.shape[1]):
                self.leak(v[:, j], kind, f"{reg}.{j}", None if tag is None else f"{tag}.{j}")
            return value
        if v.ndim != 1:
            raise ValueError("leak(): value must be a scalar, (n,) or (n, k)")
        if self.model == "hw":
            L = hw(v)
        else:
            prev = self._regs.get(reg)
            if prev is None:            # unknown earlier contents: random, fresh per trace
                bits = 8 * max(1, v.dtype.itemsize) if v.dtype.kind in "ui" else 64
                prev = self.rng.integers(0, 1 << min(bits, 63), size=self.n, dtype=np.uint64)
            L = hw(np.bitwise_xor(prev.astype(np.uint64), v.astype(np.uint64)))
            self._regs[reg] = v.copy()
        self.kinds.append(kind)
        self.tags.append(tag)
        self._L.append(np.asarray(L, dtype=np.float32))
        return value

    # ------------------------------------------------------- 8-bit instructions
    def load(self, v, tag=None, reg="r24"):
        """Load a byte from RAM into a register."""
        return self.leak(_u8(v), "load", reg, tag)

    def mov(self, v, tag=None, reg="r24"):
        """Register-to-register move (or any other plain byte copy)."""
        return self.leak(_u8(v), "mov", reg, tag)

    def xor(self, a, b, tag=None, reg="r24"):
        """a ^ b (bytes)."""
        return self.leak(np.bitwise_xor(_u8(a), _u8(b)), "xor", reg, tag)

    def xtime(self, a, tag=None, reg="r24"):
        """Multiply a byte by 2 in GF(2^8)."""
        return self.leak(XTIME[_u8(a)], "xtime", reg, tag)

    def lookup(self, table, idx, tag=None, reg="r24"):
        """table[idx].  table is (256,) (same for every trace) or (n, 256) (one table per trace)."""
        table = np.asarray(table)
        idx = np.broadcast_to(_u8(idx), (self.n,))
        v = table[idx] if table.ndim == 1 else table[self.rows, idx]
        return self.leak(_u8(v), "lookup", reg, tag)

    def store(self, v, tag=None):
        """Store a byte to RAM (drives the data bus; HD model uses register 'bus')."""
        return self.leak(_u8(v), "store", "bus", tag)

    def nop(self, k=1, tag=None):
        for _ in range(k):
            self.leak(np.zeros(self.n, np.uint8), "nop", "nop", tag)

    def random_bytes(self, k=None, tag=None, reg="r24"):
        """Read fresh random bytes from the device TRNG: (n,) if k is None else (n, k).
        Each byte read is one 'rng' instruction (it leaks like any other register write)."""
        shape = (self.n,) if k is None else (self.n, k)
        r = self.rng.integers(0, 256, size=shape, dtype=np.uint8)
        self.leak(r, "rng", reg, tag)
        return r

    def masked_table(self, sbox, m_in, m_out, tag="table"):
        """Recompute a masked S-box per trace:  T[x ^ m_in] = sbox[x] ^ m_out.
        Returns T with shape (n, 256).  The real loop has 256 iterations; to keep traces short we
        record only its first 16 stores (values sbox[j] ^ m_out, which do not depend on the key)."""
        sbox = np.asarray(sbox, dtype=np.uint8)
        m_in = np.broadcast_to(_u8(m_in), (self.n,))
        m_out = np.broadcast_to(_u8(m_out), (self.n,))
        T = sbox[np.arange(256)[None, :] ^ m_in[:, None].astype(np.intp)] ^ m_out[:, None]
        for j in range(16):
            self.store(sbox[j] ^ m_out, tag=f"{tag}[{j}]")
        return T.astype(np.uint8)

    def delay(self, max_cycles):
        """Insert a random number (uniform 0..max_cycles, fresh per trace) of dummy instructions here."""
        max_cycles = int(max_cycles)
        if max_cycles > 0:
            d = self.rng.integers(0, max_cycles + 1, size=self.n)
            self._delays.append((len(self.kinds), d, max_cycles))

    # ------------------------------------------------- big-number instructions
    def loop(self, tag=None):
        """Loop bookkeeping between big-number operations (3 cycles, data independent)."""
        for _ in range(3):
            self.leak(np.zeros(self.n, np.uint8), "loop", "pc", tag)

    def modmul(self, a, b, mod, nwords, reg="R", tag=None):
        """[(x * y) % mod for x, y in zip(a, b)] on lists of n Python ints.
        Leaks nwords operand-fetch samples (words of b) then nwords result-word samples."""
        res = [(x * y) % mod for x, y in zip(a, b)]
        self.leak(to_words(b, nwords), "mulrd", "opB", tag)
        self.leak(to_words(res, nwords), "mul", reg, tag)
        return res

    def modsqr(self, a, mod, nwords, reg="R", tag=None):
        """[(x * x) % mod for x in a].  Squaring is cheaper: only nwords result-word samples."""
        res = [(x * x) % mod for x in a]
        self.leak(to_words(res, nwords), "sqr", reg, tag)
        return res

    # ------------------------------------------------------------- rendering
    def leakage_matrix(self):
        """Noise-free data-dependent part L, shape (n, #instructions) (for debugging/grading)."""
        return np.stack(self._L, axis=1) if self._L else np.zeros((self.n, 0), np.float32)

    def tags_window(self, window=None):
        tags = list(self.tags)
        if window is None:
            return tags
        a, b = window
        tags = tags[a:b]
        return tags + [None] * max(0, (b - a) - len(tags))

    def render(self, noise=1.0, jitter=0, window=None, rng=None):
        """Turn the recorded instructions into power traces, float32 array (n, samples).

        noise   standard deviation of the additive Gaussian noise
        jitter  trigger misalignment: 0..jitter unrelated instructions before the first one
        window  (start, stop) sample range to keep (like an oscilloscope capture window);
                None keeps everything.  Samples past the end of the program are background.
        """
        rng = rng if rng is not None else self.rng
        n, nops = self.n, len(self.kinds)
        delays = list(self._delays)
        if jitter:
            delays.insert(0, (0, rng.integers(0, int(jitter) + 1, size=n), int(jitter)))
        maxtot = sum(m for _, _, m in delays)
        full = nops + maxtot
        a, b = (0, full) if window is None else (int(window[0]), int(window[1]))
        # only instructions that can land inside [a, b) need to be rendered
        lo, hi = max(0, a - maxtot), min(nops, b)
        idx = np.arange(lo, hi)
        base = np.array([KINDS[self.kinds[i]][0] for i in idx], dtype=np.float32)
        gain = np.array([KINDS[self.kinds[i]][1] for i in idx], dtype=np.float32)
        sig = (base[None, :] + gain[None, :] * np.stack([self._L[i] for i in idx], axis=1)
               if len(idx) else np.zeros((n, 0), np.float32))
        out = background(n, b - a, rng)
        if not delays:
            if len(idx):
                out[:, lo - a:hi - a] = sig
        elif len(idx):
            cum = np.zeros((n, len(idx)), dtype=np.int64)
            for at, d, _ in delays:
                cum[:, max(0, at - lo):] += d[:, None]
            pos = idx[None, :] + cum - a
            keep = (pos >= 0) & (pos < b - a)
            r = np.broadcast_to(self.rows[:, None], pos.shape)
            out[r[keep], pos[keep]] = sig[keep]
        if noise:
            out += rng.standard_normal(out.shape, dtype=np.float32) * np.float32(noise)
        return out
