"""Part 1 -- Flush+Reload through the syscall interface.

See README.md, Part 1.  You write user programs in the toy assembly (as Python
strings) and drive them from Python through the Machine API:

    prog = m.load(SRC)                       # assemble + load (cached by source)
    res  = m.run(prog, a0=...)               # run from _start until halt
    vals = m.read_u64s(prog["label"], n)     # read your own memory back

Useful assembler symbols: PROBE_BASE, PROBE_STRIDE (4096), PROBE_ENTRIES (256),
SYS_FR.  Debug with m.run(prog, trace=print) or `python3 -m specsim run file.s --trace`.
"""

# ---------------------------------------------------------------------------
# Exercise 1-1: measure hit and miss latencies and choose a threshold
# ---------------------------------------------------------------------------
NSAMPLES = 64

CALIBRATE_SRC = f"""
.data
.align 64
target:  .zero 64                  # the line you time
samples: .zero {8 * 2 * NSAMPLES}  # samples[2k] = hit time, samples[2k+1] = miss time
.text
_start:
    la      s0, target
    la      s1, samples
    li      s2, {NSAMPLES}
loop:
    # TODO: make sure `target` is cached, then time one load of it
    #         (rdcycle t1 / lb t0, 0(s0) / rdcycle t2) and store t2 - t1 at 0(s1).
    # TODO: flush `target` with clflush, time another load, store it at 8(s1).
    addi    s1, s1, 16
    addi    s2, s2, -1
    bnez    s2, loop
    halt
"""


def calibrate(m):
    """Return a latency threshold (cycles): below = cache hit, above = memory."""
    prog = m.load(CALIBRATE_SRC)
    m.run(prog)
    samples = m.read_u64s(prog["samples"], 2 * NSAMPLES)
    hits, misses = samples[0::2], samples[1::2]
    # TODO: pick a threshold between the two distributions.  Look at them first
    # (print them!).  Beware of rare "interrupt" spikes that make a hit slow.
    return 0


# ---------------------------------------------------------------------------
# Exercise 1-2: leak secret1[i] through SYS_FR
# ---------------------------------------------------------------------------
LEAK_SRC = """
.data
.align 64
times:  .zero 2048                  # times[b] = reload latency of PROBE[b * 4096]
.text
_start:                             # a0 = index of the secret byte to leak
    mv      s5, a0
    # TODO 1: flush all PROBE_ENTRIES slots (PROBE_BASE + b * PROBE_STRIDE)
    # TODO 2: call the victim: a0 = index, a7 = SYS_FR, syscall
    # TODO 3: time a load of every slot and store its latency in times[b]
    halt
"""


def leak_byte(m, i):
    """Return secret1[i] (an int 0..255)."""
    thr = m.user.get("threshold")
    if thr is None:
        thr = m.user["threshold"] = calibrate(m)
    prog = m.load(LEAK_SRC)
    # TODO: run the program with a0 = i, read `times` back and return the one
    # slot that was cached.  If the result is ambiguous (no hit, or several),
    # try again.
    return 0
