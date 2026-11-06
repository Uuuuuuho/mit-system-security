"""Pure-numpy AES-128 building blocks.

Everything works on uint8 arrays.  A *state* is an (n, 16) array holding n
independent AES states, one per row, in the FIPS-197 byte order: byte i of the
input block is state row r = i % 4, column c = i // 4.

This module has no leakage: it is the "golden model" used to check the
firmware in firmware.py and your own code.  The S-box is computed from its
definition (inverse in GF(2^8) followed by the affine map) instead of being
typed in, and selftest.py checks the FIPS-197 test vectors.
"""

import numpy as np


def gf_mul(a, b):
    """Multiply two bytes in GF(2^8) modulo x^8 + x^4 + x^3 + x + 1."""
    p = 0
    for _ in range(8):
        if b & 1:
            p ^= a
        hi = a & 0x80
        a = (a << 1) & 0xFF
        if hi:
            a ^= 0x1B
        b >>= 1
    return p


def _build_sbox():
    inv = [0] * 256
    for a in range(1, 256):
        for b in range(1, 256):
            if gf_mul(a, b) == 1:
                inv[a] = b
                break
    sbox = []
    for a in range(256):
        b = inv[a]
        s = b
        for k in range(1, 5):                      # b ^ rotl(b,1) ^ ... ^ rotl(b,4) ^ 0x63
            s ^= ((b << k) | (b >> (8 - k))) & 0xFF
        sbox.append(s ^ 0x63)
    return np.array(sbox, dtype=np.uint8)


SBOX = _build_sbox()
INV_SBOX = np.zeros(256, dtype=np.uint8)
INV_SBOX[SBOX] = np.arange(256, dtype=np.uint8)

# xtime(a) = 2*a in GF(2^8)
XTIME = np.array([((a << 1) ^ (0x1B if a & 0x80 else 0)) & 0xFF for a in range(256)], dtype=np.uint8)

# ShiftRows as a gather: new_state = state[:, SR_IDX]
#   new[r + 4c] = old[r + 4((c + r) % 4)]
SR_IDX = np.array([r + 4 * ((c + r) % 4) for c in range(4) for r in range(4)], dtype=np.intp)

RCON = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36]


def expand_key(key):
    """AES-128 key schedule.  key: 16 bytes (bytes/list/array).  Returns an (11, 16) uint8 array
    of round keys rk[0..10]; rk[0] is the cipher key itself."""
    k = [int(b) for b in bytes(bytearray(np.asarray(key, dtype=np.uint8).tolist()))]
    if len(k) != 16:
        raise ValueError("AES-128 key must be 16 bytes")
    w = [k[4 * i:4 * i + 4] for i in range(4)]
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]                                   # RotWord
            t = [int(SBOX[x]) for x in t]                      # SubWord
            t[0] ^= RCON[i // 4 - 1]
        w.append([a ^ b for a, b in zip(w[i - 4], t)])
    return np.array([sum(w[4 * r:4 * r + 4], []) for r in range(11)], dtype=np.uint8)


def shift_rows(st):
    return st[:, SR_IDX]


def mix_columns(st):
    out = np.empty_like(st)
    for c in range(4):
        a = [st[:, 4 * c + r] for r in range(4)]
        t = a[0] ^ a[1] ^ a[2] ^ a[3]
        for r in range(4):
            out[:, 4 * c + r] = a[r] ^ t ^ XTIME[a[r] ^ a[(r + 1) % 4]]
    return out


def aes_encrypt(pts, key):
    """Encrypt an (n, 16) uint8 array of plaintexts (or one 16-byte block) under one key."""
    st = np.atleast_2d(np.asarray(pts, dtype=np.uint8)).copy()
    rk = expand_key(key)
    st ^= rk[0]
    for r in range(1, 11):
        st = SBOX[st]
        st = shift_rows(st)
        if r < 10:
            st = mix_columns(st)
        st ^= rk[r]
    return st


def mix_column_masks(m):
    """MixColumns applied to one column of masks.  m: (n, 4) uint8 -> (n, 4) uint8.
    Because MixColumns is linear over GF(2), MC(a ^ m) = MC(a) ^ MC(m)."""
    m = np.asarray(m, dtype=np.uint8)
    t = m[:, 0] ^ m[:, 1] ^ m[:, 2] ^ m[:, 3]
    return np.stack([m[:, r] ^ t ^ XTIME[m[:, r] ^ m[:, (r + 1) % 4]] for r in range(4)], axis=1)
