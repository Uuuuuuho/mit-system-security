"""powersim: a small, deterministic power-trace simulator for side-channel teaching.

    leakage   Recorder (the instruction-level leakage recorder / oscilloscope), HW/HD models
    aes       pure-numpy AES-128 golden model (S-box, key schedule, ShiftRows, MixColumns)
    firmware  victim firmware written against the Recorder API (read it like a listing)
    devices   AESDevice, MaskedSboxDevice, ModExpDevice -> TraceSet
    stats     welch_t (TVLA), moving_average, sparkline, save_plot
    dvfs      toy frequency-scaling governor (power -> timing), for Discussion 4-5
"""

from .aes import SBOX, INV_SBOX, XTIME, SR_IDX, expand_key, aes_encrypt, mix_column_masks  # noqa: F401
from .leakage import Recorder, KINDS, HW8, hw, to_words                                    # noqa: F401
from .devices import TraceSet, AESDevice, MaskedSboxDevice, ModExpDevice, make_rng         # noqa: F401
from . import firmware, stats                                                              # noqa: F401

__all__ = ["SBOX", "INV_SBOX", "XTIME", "SR_IDX", "expand_key", "aes_encrypt", "mix_column_masks",
           "Recorder", "KINDS", "HW8", "hw", "to_words",
           "TraceSet", "AESDevice", "MaskedSboxDevice", "ModExpDevice", "make_rng", "firmware", "stats"]
