"""Simulated target devices.  Each one holds a secret and hands out trace sets.

    dev = AESDevice(seed=1, noise=2.0)
    ts = dev.capture(1000)            # TraceSet: ts.traces (1000, S) float32, ts.pts, ts.cts, ts.tags

Attributes starting with an underscore (dev._key, dev._e) are the secrets: the
grader reads them, you must not.
"""

import numpy as np

from . import firmware
from .aes import aes_encrypt, expand_key
from .leakage import Recorder


def make_rng(seed=None):
    return np.random.default_rng(seed)


class TraceSet:
    """traces (n, S) float32; pts/cts (n, 16) uint8 or None; inputs/outputs: lists of ints (modexp);
    tags: list of S strings naming the instruction at each sample *when there is no misalignment*
    (None = no instruction / background); meta: dict of device parameters."""

    def __init__(self, traces, pts=None, cts=None, inputs=None, outputs=None, tags=None, meta=None):
        self.traces = traces
        self.pts = pts
        self.cts = cts
        self.inputs = inputs
        self.outputs = outputs
        self.tags = tags
        self.meta = dict(meta or {})

    def __len__(self):
        return self.traces.shape[0]

    def __getitem__(self, sl):
        """ts[:100] -> a TraceSet with the first 100 traces."""
        pick = (lambda x: None if x is None else x[sl])
        return TraceSet(self.traces[sl], pick(self.pts), pick(self.cts), pick(self.inputs),
                        pick(self.outputs), self.tags, self.meta)

    def index(self, tag):
        """Sample index of the instruction tagged `tag` (nominal, i.e. without jitter/delays)."""
        return self.tags.index(tag)

    def save(self, path):
        np.savez_compressed(path, traces=self.traces,
                            pts=self.pts if self.pts is not None else np.zeros((0, 16), np.uint8),
                            cts=self.cts if self.cts is not None else np.zeros((0, 16), np.uint8),
                            tags=np.array([t or "" for t in (self.tags or [])]))


class AESDevice:
    """8-bit software AES-128 (firmware.aes_firmware) with a fixed secret key.

    model        'hw' or 'hd' leakage
    noise        Gaussian noise standard deviation (unit: one bit of Hamming weight)
    jitter       trigger misalignment, 0..jitter samples
    round_delay  random delay countermeasure: 0..round_delay dummy instructions before each round
    shuffle      shuffling countermeasure: True (S-box loops + ShiftRows moves) or "sbox" (loops only)
    window       (start, stop) capture window in samples; None = whole encryption
    """

    def __init__(self, seed=None, rng=None, key=None, model="hw", noise=1.0, jitter=0,
                 round_delay=0, shuffle=False, window=(0, 200)):
        self.rng = rng if rng is not None else make_rng(seed)
        if key is None:
            key = self.rng.integers(0, 256, 16, dtype=np.uint8)
        self._key = np.array(key, dtype=np.uint8)
        self._rk = expand_key(self._key)
        self.model, self.noise, self.jitter = model, noise, jitter
        self.round_delay, self.shuffle, self.window = round_delay, shuffle, window

    def encrypt(self, pts):
        return aes_encrypt(pts, self._key)

    def _run(self, cpu, pts):
        return firmware.aes_firmware(cpu, pts, self._rk, shuffle=self.shuffle,
                                     round_delay=self.round_delay)

    def capture(self, n, pts=None):
        if pts is None:
            pts = self.rng.integers(0, 256, (n, 16), dtype=np.uint8)
        pts = np.asarray(pts, dtype=np.uint8)
        cpu = Recorder(len(pts), self.model, rng=self.rng)
        cts = self._run(cpu, pts)
        traces = cpu.render(self.noise, jitter=self.jitter, window=self.window, rng=self.rng)
        meta = dict(device=type(self).__name__, model=self.model, noise=self.noise, jitter=self.jitter,
                    round_delay=self.round_delay, shuffle=self.shuffle, window=self.window)
        return TraceSet(traces, pts=pts, cts=cts, tags=cpu.tags_window(self.window), meta=meta)


class MaskedSboxDevice(AESDevice):
    """First-order masked AES; the capture window covers mask generation and the first-round
    AddRoundKey+SubBytes loop only (firmware.masked_sbox_firmware).  cts are computed by the
    reference AES (the masked loop's outputs are masked)."""

    def __init__(self, seed=None, rng=None, key=None, model="hw", noise=1.0, jitter=0):
        super().__init__(seed=seed, rng=rng, key=key, model=model, noise=noise, jitter=jitter,
                         window=None)

    def _run(self, cpu, pts):
        firmware.masked_sbox_firmware(cpu, pts, self._rk)
        return self.encrypt(pts)


class ModExpDevice:
    """Square-and-multiply modular exponentiation with a secret nbits-bit exponent (MSB set)
    and a public mod_bits-bit odd modulus."""

    def __init__(self, seed=None, rng=None, nbits=128, mod_bits=256, model="hw", noise=1.0, jitter=0):
        self.rng = rng if rng is not None else make_rng(seed)
        self.nbits, self.mod_bits, self.nwords = nbits, mod_bits, mod_bits // 32
        self._e = int.from_bytes(self.rng.bytes(nbits // 8), "big") | (1 << (nbits - 1))
        self.modulus = int.from_bytes(self.rng.bytes(mod_bits // 8), "big") | (1 << (mod_bits - 1)) | 1
        self.model, self.noise, self.jitter = model, noise, jitter

    def capture(self, n, bases=None):
        if bases is None:
            bases = [int.from_bytes(self.rng.bytes(self.mod_bits // 8), "big") % self.modulus
                     for _ in range(n)]
        cpu = Recorder(len(bases), self.model, rng=self.rng)
        out = firmware.modexp_sqm(cpu, bases, self._e, self.modulus, self.nbits, self.nwords)
        traces = cpu.render(self.noise, jitter=self.jitter, rng=self.rng)
        meta = dict(device="ModExpDevice", nbits=self.nbits, modulus=self.modulus, nwords=self.nwords,
                    model=self.model, noise=self.noise, jitter=self.jitter)
        # no tags: they would give the exponent away
        return TraceSet(traces, inputs=list(bases), outputs=out, tags=None, meta=meta)
