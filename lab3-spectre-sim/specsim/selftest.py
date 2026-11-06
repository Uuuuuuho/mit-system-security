"""Sanity tests for the simulator itself (run by `make` and `python3 -m specsim selftest`)."""

from . import asmtools
from .asm import AsmError, assemble
from .cpu import ProgramFault
from .layout import PUBLIC_CONSTANTS, KTEXT_BASE, KERNEL_BASE
from .machine import Machine, kernel_source

M64 = (1 << 64) - 1


def _expect_fault(m, src, what):
    try:
        m.run(m.load(src))
    except ProgramFault:
        return
    raise AssertionError(f"expected a fault: {what}")


def test_alu_mem_control():
    m = Machine(seed=1)
    p = m.load("""
.data
.align 64
buf:   .quad 0x1122334455667788, 0
bytes: .byte 0xff, 1, 2, 3
.text
_start:
    li   t0, 7
    li   t1, -3
    add  s2, t0, t1          # 4
    sub  s3, t1, t0          # -10
    mul  s4, t0, t0          # 49
    divu s5, s4, t0          # 7
    remu s6, s4, t1          # 49 % (2^64-3) = 49
    slt  s7, t1, t0          # 1 (signed)
    sltu s8, t1, t0          # 0 (unsigned)
    srai s9, t1, 1           # -2
    srli s10, t1, 60         # 15
    la   a1, buf
    ld   a2, 0(a1)
    lw   a3, 4(a1)           # 0x11223344
    lb   a4, 7(a1)           # 0x11 (zero-extended)
    la   a5, bytes
    lb   a6, 0(a5)           # 0xff, not sign-extended
    sb   t0, 8(a1)
    sw   t1, 12(a1)
    ld   s11, 8(a1)          # 0xfffffffd_00000007
    call func
    bnez a0, out
    li   a0, 99
out:
    halt
func:
    li   a0, 5
    ret
""")
    r = m.run(p)
    exp = {"s2": 4, "s3": (-10) & M64, "s4": 49, "s5": 7, "s6": 49, "s7": 1, "s8": 0,
           "s9": (-2) & M64, "s10": 15, "a2": 0x1122334455667788, "a3": 0x11223344,
           "a4": 0x11, "a6": 0xff, "s11": 0xfffffffd00000007, "a0": 5}
    for k, v in exp.items():
        assert r[k] == v, f"{k} = {r[k]:#x}, expected {v:#x}"


def test_syscalls_and_privilege():
    m = Machine(seed=2)
    r = m.run(m.load("_start:\n li a7, SYS_NOP\n syscall\n mv s2, a0\n li a7, 77\n syscall\n halt\n"))
    assert r["s2"] == 0 and r["a0"] == M64, "SYS_NOP / unknown syscall return values"
    _expect_fault(m, "_start:\n li t0, k_secret1\n lb t1, 0(t0)\n halt\n", "user load from kernel")
    _expect_fault(m, "_start:\n li t0, k_v1_limit\n clflush 0(t0)\n halt\n", "user clflush of kernel line")
    _expect_fault(m, "_start:\n li t0, 0x80000000\n jr t0\n halt\n", "user jump into kernel text")
    _expect_fault(m, "_start:\n sysret\n", "sysret in user mode")
    _expect_fault(m, "_start:\n li t0, 16\n ld t1, 0(t0)\n halt\n", "load from unmapped page")
    try:
        m.load("_start:\n bogus t0, t1\n")
        raise AssertionError("assembler accepted a bogus instruction")
    except AsmError:
        pass


def test_timing():
    m = Machine(seed=3)
    p = m.load("""
.data
.align 64
x: .zero 64
.text
_start:
    la   s0, x
    lb   t0, 0(s0)
    rdcycle t1
    lb   t0, 0(s0)
    rdcycle t2
    sub  s2, t2, t1          # hit latency
    clflush 0(s0)
    rdcycle t1
    lb   t0, 0(s0)
    rdcycle t2
    sub  s3, t2, t1          # miss latency
    halt
""")
    hits, misses = [], []
    for _ in range(20):
        r = m.run(p)
        hits.append(r["s2"])
        misses.append(r["s3"])
    hits.sort()
    misses.sort()
    assert hits[10] < 20, f"hit latency too high: {hits}"
    assert misses[10] > 100, f"miss latency too low: {misses}"


def test_speculation_leaves_footprint():
    """A user-mode bounds-check bypass: the wrong path loads a line, the squash
    restores registers and memory, but the line stays cached."""
    m = Machine(seed=4)
    p = m.load("""
.data
.align 64
limit:  .quad 4
.align 64
flag:   .quad 0
.align 64
target: .zero 64
.text
_start:                      # a0 = index, a1 = 1 to flush the bound first
    la   s0, limit
    beqz a1, nofl
    clflush 0(s0)
nofl:
    ld   t0, 0(s0)
    bgeu a0, t0, skip
    la   t1, target
    lb   t2, 0(t1)           # only reached (architecturally) when a0 < limit
    la   t3, flag
    li   t4, 1
    sd   t4, 0(t3)           # transient store must not reach memory
    mv   s2, t4              # transient register write must be undone
skip:
    halt
""")
    for _ in range(4):
        m.run(p, a0=1, a1=0)                     # train: in bounds
    m.write(p["flag"], bytes(8))
    m._cpu.caches.flush(p["target"])
    r = m.run(p, a0=100, a1=1)                   # out of bounds, cold bound
    assert m._cpu.caches.level_of(p["target"]) < 3, "wrong-path load left no cache footprint"
    assert m.read_u64(p["flag"]) == 0, "wrong-path store reached memory"
    assert r["s2"] == 0, "wrong-path register write was not squashed"
    # with a fence after the branch the footprint disappears
    m2 = Machine(seed=4)
    p2 = m2.load(p_src_with_fence())
    for _ in range(4):
        m2.run(p2, a0=1, a1=0)
    m2._cpu.caches.flush(p2["target"])
    m2.run(p2, a0=100, a1=1)
    assert m2._cpu.caches.level_of(p2["target"]) == 3, "fence did not stop speculation"


def p_src_with_fence():
    return """
.data
.align 64
limit:  .quad 4
.align 64
target: .zero 64
.text
_start:
    la   s0, limit
    beqz a1, nofl
    clflush 0(s0)
nofl:
    ld   t0, 0(s0)
    bgeu a0, t0, skip
    fence
    la   t1, target
    lb   t2, 0(t1)
skip:
    halt
"""


def test_kernel_fr_touches_probe():
    m = Machine(seed=5)
    secret = m._secret(1)
    probe = PUBLIC_CONSTANTS["PROBE_BASE"]
    for i in (0, 7):
        for b in range(256):
            m._cpu.caches.flush(probe + 4096 * b)
        m.run(m.load("_start:\n li a7, SYS_FR\n syscall\n halt\n"), a0=i)
        cached = [b for b in range(256) if m._cpu.caches.level_of(probe + 4096 * b) < 3]
        assert cached == [secret[i]], f"SYS_FR touched {cached}, expected [{secret[i]}]"


def test_asmtools_roundtrip():
    src = kernel_source()
    a = assemble(src, KTEXT_BASE, KERNEL_BASE, PUBLIC_CONSTANTS)
    b = assemble(asmtools.render(asmtools.parse(src)), KTEXT_BASE, KERNEL_BASE, PUBLIC_CONSTANTS)
    strip = lambda code: [ins[:5] for ins in code]
    assert strip(a.code) == strip(b.code) and a.data == b.data, "parse/render changed the kernel"


TESTS = [test_alu_mem_control, test_syscalls_and_privilege, test_timing,
         test_speculation_leaves_footprint, test_kernel_fr_touches_probe, test_asmtools_roundtrip]


def main():
    ok = True
    for t in TESTS:
        try:
            t()
            print(f"  ok    {t.__name__}")
        except Exception as e:      # report every failing test
            ok = False
            print(f"  FAIL  {t.__name__}: {e}")
    print("specsim selftest:", "PASS" if ok else "FAIL")
    return 0 if ok else 1
