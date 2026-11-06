#!/usr/bin/env python3
"""fuzz.py - Part 4: differential fuzzing of psp-lite against its golden model
(STARTER).

Loop:
  1. generate a random straight-line-ish RV32I program (forward branches/jumps
     only, loads/stores relative to x31 into a private data block) and random
     initial register values biased towards "interesting" numbers;
  2. run it on the CPU under test and on `psp-lite --golden`, both with --trace;
  3. the first trace line that differs is the first instruction whose effect
     differs -> classify it by mnemonic;
  4. rebuild the architectural state just before that instruction from the
     golden trace (initial registers + every register/memory write) and make a
     one-instruction reproducer; keep the finding only if the reproducer still
     diverges;
  5. track decoder coverage: which of the 37 RV32I mnemonics actually executed.

Output (parsed by the grader):
  FOUND <mnemonic> insn=0x<word> ...      one line per distinct mnemonic found
  SUMMARY tests=<n> instructions=<n> coverage=<k>/37 found=<list>
"""
import argparse
import os
import random
import sys
import tempfile
import time


def _find_lab_part4():
    d = os.path.dirname(os.path.abspath(__file__))
    for _ in range(4):
        if os.path.exists(os.path.join(d, "part4", "rv32.py")):
            return os.path.join(d, "part4")
        d = os.path.dirname(d)
    raise SystemExit("cannot find part4/rv32.py")


sys.path.insert(0, _find_lab_part4())
import rv32  # noqa: E402

INTERESTING = [0, 1, 2, 0x7FF, 0x800, 0xFFF, 0x1000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE,
               0x0000FFFF, 0xFFFF0000, 0x00008000, 0xFFFF8000, 0x55555555, 0xAAAAAAAA]
IMM_INTERESTING = [0, 1, -1, 2, -2, 2047, -2048, 0x7F0, -0x800 + 16, 31, 32]


def rand_value(rng):
    r = rng.random()
    if r < 0.35:
        return rng.choice(INTERESTING)
    if r < 0.55:
        return rng.randrange(0, 0x1000)
    if r < 0.65:
        return (-rng.randrange(1, 0x1000)) & 0xFFFFFFFF
    return rng.getrandbits(32)


def rand_imm(rng):
    return rng.choice(IMM_INTERESTING) if rng.random() < 0.3 else rng.randrange(-2048, 2048)


def rd_reg(rng):
    return rng.randrange(0, 31)  # x0..x30: never clobber the data pointer x31


def any_reg(rng):
    return rng.randrange(0, 32)


def gen_program(rng, n):
    """TODO (Exercise 4-1): return (body, regs, data): a list of n+ instruction words that
    exercises many different RV32I mnemonics (use rv32.encode; keep control flow forward-only
    and memory accesses relative to rv32.BASE_REG inside the data block), 31 initial register
    values (bias them towards interesting numbers), and rv32.DATA_SIZE bytes of data."""
    body = [rv32.encode("addi", rd=1, rs1=1, imm=1) for _ in range(n)]
    regs = [0] * 31
    data = bytes(rv32.DATA_SIZE)
    return body, regs, data


def first_divergence(golden, dut):
    """TODO (Exercise 4-2): index of the first differing trace line, or None."""
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cpu", default=rv32.find_cpu(), help="psp-lite binary under test")
    ap.add_argument("--tests", type=int, default=400)
    ap.add_argument("--length", type=int, default=300, help="body instructions per test")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--time-budget", type=float, default=120.0, help="seconds")
    a = ap.parse_args()
    seed = a.seed if a.seed is not None else random.randrange(1 << 30)
    rng = random.Random(seed)
    print(f"fuzz: cpu={a.cpu} seed={seed} tests={a.tests} length={a.length}")
    covered, found = set(), {}
    t0, ntests, ninstr = time.time(), 0, 0
    with tempfile.TemporaryDirectory() as tmp:
        img, tg, td = (os.path.join(tmp, n) for n in ("t.bin", "g.txt", "d.txt"))
        for ntests in range(1, a.tests + 1):
            if time.time() - t0 > a.time_budget:
                ntests -= 1
                break
            body, regs, data = gen_program(rng, a.length)
            with open(img, "wb") as f:
                f.write(rv32.flat_image(body, regs, data))
            rv32.run_iss(a.cpu, img, golden=True, trace=tg)
            rv32.run_iss(a.cpu, img, golden=False, trace=td)
            g, d = rv32.read_trace(tg), rv32.read_trace(td)
            ninstr += len(g)
            for line in g[rv32.PROLOGUE_WORDS:]:
                t = rv32.parse_trace_line(line)
                if t and t["mnemonic"] in rv32.INSNS:
                    covered.add(t["mnemonic"])
            i = first_divergence(g, d)
            if i is None:
                continue
            # TODO (Exercise 4-3): classify the divergence by the golden instruction's mnemonic
            # (rv32.decode), build a ONE-instruction reproducer from the golden state just before it,
            # keep it only if the reproducer still diverges, and print
            #   FOUND <mnemonic> insn=0x<word> ...
            # once per distinct mnemonic (store it in `found`).
    miss = [m for m in rv32.MNEMONICS if m not in covered]
    print(f"coverage: {len(covered)}/{len(rv32.MNEMONICS)} mnemonics" + (f" (missing: {' '.join(miss)})" if miss else ""))
    print(f"SUMMARY tests={ntests} instructions={ninstr} coverage={len(covered)}/{len(rv32.MNEMONICS)} "
          f"found={','.join(sorted(found)) or 'none'} time={time.time() - t0:.1f}s")


if __name__ == "__main__":
    main()
