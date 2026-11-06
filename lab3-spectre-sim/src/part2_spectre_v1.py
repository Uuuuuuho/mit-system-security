"""Part 2 -- Spectre v1 through SYS_V1.

See README.md, Part 2.  The kernel's SYS_V1(x) does

    if (x < v1_limit)                       // v1_limit is flushed on every call
        touch PROBE[v1_array[x] * 4096]

and the secret you want, secret2, lives in kernel memory at k_secret2, after
v1_array (both addresses are in m.ksyms and are also assembler symbols).
You cannot read k_secret2 from user mode -- but the kernel can, speculatively.

You may copy code from your Part 1 solution.
"""

# Suggested register convention: pass per-run values in registers instead of
# generating a new program per byte, e.g. m.run(prog, a0=x, a1=train_x).
ATTACK_SRC = """
.data
.align 64
times:  .zero 2048                  # times[b] = reload latency of PROBE[b * 4096]
.text
_start:                             # a0 = malicious x, a1 = in-bounds x (your choice)
    mv      s5, a0
    mv      s6, a1
    # TODO 1: train the bounds-check branch: several SYS_V1 calls with an in-bounds x
    # TODO 2: flush the probe array
    # TODO 3: one SYS_V1 call with the out-of-bounds x
    # TODO 4: reload and time every probe slot into times[]
    halt
"""


def leak_byte(m, i):
    """Return secret2[i] (an int 0..255)."""
    x = m.ksyms["k_secret2"] - m.ksyms["k_v1_array"] + i   # out-of-bounds index
    prog = m.load(ATTACK_SRC)
    # TODO: run the attack (maybe several times), decode `times`, return the byte.
    #       You need a hit/miss threshold: reuse your Part 1 calibration code.
    return 0
