"""Self-tests for powersim (run with `python3 -m powersim selftest`)."""

import numpy as np

from . import firmware
from .aes import SBOX, INV_SBOX, XTIME, aes_encrypt, expand_key, gf_mul, mix_column_masks, mix_columns
from .devices import AESDevice, MaskedSboxDevice, ModExpDevice
from .leakage import KINDS, Recorder, hw


def t_fips197():
    k = np.arange(16, dtype=np.uint8)
    p = np.frombuffer(bytes.fromhex("00112233445566778899aabbccddeeff"), np.uint8)
    assert bytes(aes_encrypt(p, k)[0]).hex() == "69c4e0d86a7b0430d8cdb78070b4c55a"
    k = np.frombuffer(bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c"), np.uint8)
    p = np.frombuffer(bytes.fromhex("3243f6a8885a308d313198a2e0370734"), np.uint8)
    assert bytes(aes_encrypt(p, k)[0]).hex() == "3925841d02dc09fbdc118597196a0b32"
    assert bytes(expand_key(k)[10]).hex() == "d014f9a8c9ee2589e13f0cc8b6630ca6"


def t_sbox():
    assert SBOX[0x00] == 0x63 and SBOX[0x53] == 0xED and len(set(SBOX.tolist())) == 256
    assert all(INV_SBOX[SBOX[x]] == x for x in range(256))
    assert all(XTIME[a] == gf_mul(a, 2) for a in range(256))


def t_mixcolumn_linear():
    rng = np.random.default_rng(0)
    st = rng.integers(0, 256, (50, 16), dtype=np.uint8)
    m = rng.integers(0, 256, (50, 4), dtype=np.uint8)
    mm = np.tile(m, 4)
    lhs = mix_columns(st ^ mm)
    rhs = mix_columns(st) ^ np.tile(mix_column_masks(m), 4)
    assert (lhs == rhs).all()


def t_hw_hd():
    assert hw(np.array([0, 1, 255, 0x80], np.uint8)).tolist() == [0, 1, 8, 1]
    assert hw(np.array([0xFFFFFFFF, 0x10001], np.uint32)).tolist() == [32, 2]
    cpu = Recorder(3, "hd")
    cpu.mov(np.array([0x00, 0xFF, 0x0F], np.uint8))
    cpu.mov(np.array([0xFF, 0xFF, 0xF0], np.uint8))
    L = cpu.leakage_matrix()
    assert ((0 <= L[:, 0]) & (L[:, 0] <= 8)).all() and L[:, 1].tolist() == [8, 0, 8]   # 1st write: random prior


def t_render_noise_free():
    cpu = Recorder(4, "hw", rng=np.random.default_rng(1))
    cpu.nop(2)
    cpu.load(np.array([0, 1, 3, 255], np.uint8))
    tr = cpu.render(noise=0.0)
    b, g = KINDS["load"]
    assert tr.shape == (4, 3) and np.allclose(tr[:, 2], b + g * np.array([0, 1, 2, 8]))
    assert np.allclose(tr[:, :2], 0)


def t_render_delays():
    rng = np.random.default_rng(2)
    cpu = Recorder(200, "hw", rng=rng)
    cpu.nop(3)
    cpu.delay(5)
    cpu.lookup(SBOX, np.zeros(200, np.uint8))        # value 0x63 -> HW 4
    want = KINDS["lookup"][0] + 4
    d = cpu._delays[0][1]
    rows = np.arange(200)
    tr = cpu.render(noise=0.0)                       # lookup at 3 + delay, nops at 0..2
    assert tr.shape == (200, 4 + 5)
    assert np.allclose(tr[:, :3], 0) and np.allclose(tr[rows, 3 + d], want)
    tr = cpu.render(noise=0.0, jitter=4)             # background (>= 2.0) before the nops
    assert tr.shape == (200, 4 + 9)
    j = np.argmax(tr == 0, axis=1)                   # first nop = trigger jitter
    assert (j <= 4).all() and np.allclose(tr[rows, j + 3 + d], want)


def t_firmware_matches_reference():
    dev = AESDevice(seed=3, window=None)
    ts = dev.capture(64)
    assert (ts.cts == dev.encrypt(ts.pts)).all()
    dev = AESDevice(seed=4, shuffle=True, round_delay=7, jitter=5)
    ts = dev.capture(64)
    assert (ts.cts == dev.encrypt(ts.pts)).all()
    dev = AESDevice(seed=5, model="hd")
    ts = dev.capture(16)
    assert ts.traces.shape == (16, 200) and ts.tags[8] == "r1.load[0]" and ts.tags[10] == "r1.sbox[0]"


def t_masked_sbox_firmware():
    rng = np.random.default_rng(6)
    key = rng.integers(0, 256, 16, dtype=np.uint8)
    rk = expand_key(key)
    pts = rng.integers(0, 256, (100, 16), dtype=np.uint8)
    cpu = Recorder(100, "hw", rng=rng)
    out, m_out = firmware.masked_sbox_firmware(cpu, pts, rk)
    assert ((out ^ m_out[:, None]) == SBOX[pts ^ key]).all()
    dev = MaskedSboxDevice(seed=7)
    ts = dev.capture(10)
    assert ts.tags[26 + 3] == "r1.sbox[0]" and ts.tags.index("mask.out") == 9


def t_modexp():
    dev = ModExpDevice(seed=8, nbits=32, mod_bits=64)
    ts = dev.capture(4)
    assert all(o == pow(b, dev._e, dev.modulus) for b, o in zip(ts.inputs, ts.outputs))
    ones = bin(dev._e).count("1")
    assert ts.traces.shape[1] == 16 + 32 * (3 + 2) + ones * 4 + 16


TESTS = [t_fips197, t_sbox, t_mixcolumn_linear, t_hw_hd, t_render_noise_free, t_render_delays,
         t_firmware_matches_reference, t_masked_sbox_firmware, t_modexp]


def run(verbose=True):
    bad = 0
    for t in TESTS:
        try:
            t()
            if verbose:
                print(f"  ok    {t.__name__}")
        except Exception as e:          # report every failing test, not just the first
            bad += 1
            print(f"  FAIL  {t.__name__}: {type(e).__name__}: {e}")
    print(f"powersim selftest: {len(TESTS) - bad}/{len(TESTS)} passed")
    return bad == 0
