"""Part 3 -- Spectre v1 with a short speculation window.

See README.md, Part 3.  SYS_V1_HARD(x) is the same gadget as SYS_V1, except
that the kernel no longer flushes its bound (k_v1h_limit), and the index goes
through a slow dependent computation before the array load.  The secret is
secret3 at k_secret3, after k_v1h_array.

First run your Part 2 attack against SYS_V1_HARD and explain why it fails
(Discussion 3-1).  Then widen the window.  User code may not clflush a kernel
line, but nothing stops it from loading its own memory...
"""

# ---------------------------------------------------------------------------
# Exercise 3-1: an eviction set for a kernel line
# ---------------------------------------------------------------------------
def eviction_set(m, target):
    """Return a list of user-memory addresses such that loading each of them
    once (in list order) evicts the line containing `target` from L1 and L2.

    m.geometry describes the caches; m.HEAP_BASE..m.HEAP_END is free user
    memory you can use.  Keep the list short (<= 256 entries)."""
    # TODO
    return []


# ---------------------------------------------------------------------------
# Exercise 3-2: leak secret3[i] through SYS_V1_HARD
# ---------------------------------------------------------------------------
ATTACK_SRC = """
.data
.align 64
times:  .zero 2048
.text
_start:
    # TODO: train, flush the probe array, evict k_v1h_limit, attack, reload.
    #       Think about the order of these steps.  (Tip: put the eviction-set
    #       addresses in a .data array and fill it from Python with m.write_u64s.)
    halt
"""


def leak_byte(m, i):
    """Return secret3[i] (an int 0..255)."""
    x = m.ksyms["k_secret3"] - m.ksyms["k_v1h_array"] + i
    prog = m.load(ATTACK_SRC)
    # TODO
    return 0
