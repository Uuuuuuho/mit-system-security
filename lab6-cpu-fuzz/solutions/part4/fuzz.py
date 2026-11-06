#!/usr/bin/env python3
"""fuzz.py - Part 4: differential fuzzing of psp-lite against its golden model
(REFERENCE SOLUTION - spoiler).

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


def gen_block(rng, name):
    """Return a list of words implementing one occurrence of `name` (plus helpers for control flow)."""
    fmt = rv32.INSNS[name][0]
    nop = rv32.encode("addi")
    if name in ("lb", "lbu", "lh", "lhu", "lw"):
        size = {"lb": 1, "lbu": 1, "lh": 2, "lhu": 2, "lw": 4}[name]
        off = rng.randrange(-2048, 2048 - size + 1) & ~(size - 1)
        return [rv32.encode(name, rd=rd_reg(rng), rs1=rv32.BASE_REG, imm=off)]
    if name in ("sb", "sh", "sw"):
        size = {"sb": 1, "sh": 2, "sw": 4}[name]
        off = rng.randrange(-2048, 2048 - size + 1) & ~(size - 1)
        return [rv32.encode(name, rs1=rv32.BASE_REG, rs2=any_reg(rng), imm=off)]
    if fmt == "B":  # forward branch that may skip one instruction
        return [rv32.encode(name, rs1=any_reg(rng), rs2=any_reg(rng), imm=8), nop]
    if name == "jal":
        return [rv32.encode("jal", rd=rd_reg(rng), imm=8), nop]
    if name == "jalr":  # auipc rT,0 ; jalr rd, 12(rT) -> lands after the skipped slot
        rt = rng.randrange(1, 31)
        return [rv32.encode("auipc", rd=rt, imm=0), rv32.encode("jalr", rd=rd_reg(rng), rs1=rt, imm=12), nop]
    if fmt == "U":
        return [rv32.encode(name, rd=rd_reg(rng), imm=rng.getrandbits(20))]
    if fmt == "SH":
        return [rv32.encode(name, rd=rd_reg(rng), rs1=any_reg(rng), imm=rng.randrange(32))]
    if fmt == "I":
        return [rv32.encode(name, rd=rd_reg(rng), rs1=any_reg(rng), imm=rand_imm(rng))]
    return [rv32.encode(name, rd=rd_reg(rng), rs1=any_reg(rng), rs2=any_reg(rng))]


def gen_program(rng, n):
    body = []
    while len(body) < n:
        body += gen_block(rng, rng.choice(rv32.MNEMONICS))
    regs = [rand_value(rng) for _ in range(31)]
    data = bytes(rng.getrandbits(8) for _ in range(rv32.DATA_SIZE))
    return body, regs, data


def first_divergence(golden, dut):
    """Index of the first differing trace line (or None if identical)."""
    for i, (a, b) in enumerate(zip(golden, dut)):
        if a != b:
            return i
    if len(golden) != len(dut):
        return min(len(golden), len(dut))
    return None


def state_before(golden_lines, idx, regs0, data0):
    """Replay the golden trace up to line idx: registers and data block before that instruction."""
    regs = [0] + list(regs0)
    mem = bytearray(data0)
    for line in golden_lines[:idx]:
        t = rv32.parse_trace_line(line)
        if not t or t["trap"]:
            continue
        if t["rd"] is not None:
            regs[t["rd"]] = t["rd_val"]
        if t["mem_val"] is not None:
            size = {"sb": 1, "sh": 2, "sw": 4}.get(t["mnemonic"], 4)
            off = t["mem"] - (rv32.BASE + rv32.DATA_OFF)
            if 0 <= off <= rv32.DATA_SIZE - size:
                mem[off:off + size] = t["mem_val"].to_bytes(4, "little")[:size]
    regs[0] = 0
    return regs, bytes(mem)


def diverges(cpu, tmp, body, regs, data):
    img = os.path.join(tmp, "repro.bin")
    with open(img, "wb") as f:
        f.write(rv32.flat_image(body, regs, data))
    tg, td = os.path.join(tmp, "rg.txt"), os.path.join(tmp, "rd.txt")
    rv32.run_iss(cpu, img, golden=True, trace=tg)
    rv32.run_iss(cpu, img, golden=False, trace=td)
    return first_divergence(rv32.read_trace(tg), rv32.read_trace(td)) is not None


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
            tg_line = rv32.parse_trace_line(g[i]) if i < len(g) else None
            if tg_line is None:
                continue
            dec = rv32.decode(tg_line["word"])
            name = dec["name"]
            if name in found:
                continue
            regs_i, data_i = state_before(g, i, regs[:], data)
            repro = diverges(a.cpu, tmp, [tg_line["word"]], regs_i[1:], data_i)
            if not repro:
                continue
            td_line = rv32.parse_trace_line(d[i]) if i < len(d) else None
            found[name] = tg_line["word"]
            print(f"FOUND {name} insn=0x{tg_line['word']:08x} test={ntests} pc=0x{tg_line['pc']:08x} "
                  f"rs1=0x{regs_i[dec['rs1']]:08x} rs2=0x{regs_i[dec['rs2']]:08x} imm={dec['imm']} "
                  f"golden: {g[i].split(None, 4)[4].strip()} | dut: {d[i].split(None, 4)[4].strip() if td_line else '?'}",
                  flush=True)
    miss = [m for m in rv32.MNEMONICS if m not in covered]
    print(f"coverage: {len(covered)}/{len(rv32.MNEMONICS)} mnemonics" + (f" (missing: {' '.join(miss)})" if miss else ""))
    print(f"SUMMARY tests={ntests} instructions={ninstr} coverage={len(covered)}/{len(rv32.MNEMONICS)} "
          f"found={','.join(sorted(found)) or 'none'} time={time.time() - t0:.1f}s")


if __name__ == "__main__":
    main()
