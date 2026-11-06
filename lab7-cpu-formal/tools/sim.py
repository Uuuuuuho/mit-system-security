#!/usr/bin/env python3
"""Run the test programs on psp-mini with iverilog, co-simulating the spec.

    sim.py [--which src|solutions] [--defects glitch,backdoor] [--mutant N]
           [--nospec] [--trace] [programs/x.s ...]

For every program: assemble it, run it on the implementation (sim/tb.v) with
the spec core stepping in lock-step, then
  * check the '#!' expectations in the program against the final state, and
  * report the first implementation/spec mismatch (unless --nospec).
Exit status 0 only if everything passed.
"""
import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LAB = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import rvasm  # noqa: E402


def defines(defects=(), mutant=0):
    d = []
    if "glitch" in defects:
        d.append("PSP_GLITCH")
    if "backdoor" in defects:
        d.append("PSP_BACKDOOR")
    if mutant:
        d.append(f"PSP_MUTANT={mutant}")
    return d


def build(which, defs_dir, out_dir, defects=(), mutant=0):
    """Compile the testbench; return the .vvp path (or raise RuntimeError)."""
    os.makedirs(out_dir, exist_ok=True)
    tag = "_".join([os.path.basename(os.path.normpath(which))] + sorted(defects) +
                   ([f"m{mutant}"] if mutant else []))
    vvp = os.path.join(out_dir, f"tb_{tag}.vvp")
    cmd = ["iverilog", "-g2005", "-Wall", "-Wno-timescale", "-I", os.path.join(LAB, "rtl"),
           "-I", defs_dir, "-o", vvp]
    for d in defines(defects, mutant):
        cmd.append("-D" + d)
    cmd += [os.path.join(LAB, "sim", "tb.v"), os.path.join(LAB, "rtl", "psp_mini.v"),
            os.path.join(which, "spec_core.v")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    msgs = [l for l in (r.stdout + r.stderr).splitlines()
            if l.strip() and "sorry: constant selects" not in l]
    if r.returncode != 0:
        raise RuntimeError("iverilog failed:\n" + "\n".join(msgs))
    return vvp


def expectations(path):
    exp = {}
    for line in open(path):
        if line.startswith("#!"):
            for tok in line[2:].split():
                k, v = tok.split("=")
                if v in ("U", "M"):
                    v = "1" if v == "M" else "0"
                exp[k] = int(v, 0)
    return exp


def parse_final(text):
    m = re.search(r"^FINAL (.*)$", text, re.M)
    if not m:
        return None
    st = {}
    for tok in m.group(1).split():
        k, v = tok.split("=")
        st[k] = int(v, 16) if k in ("pc", "mtvec", "mepc") or re.fullmatch(r"(x|mem)\d+", k) \
            else int(v)
    return st


def run_program(vvp, prog, out_dir, nospec=False, trace=False, maxcycles=20000):
    """Return dict(ok, expect_ok, cosim_ok, lines, final, problems)."""
    hexf = os.path.join(out_dir, os.path.basename(prog)[:-2] + ".hex")
    words = rvasm.assemble(open(prog).read())
    with open(hexf, "w") as f:
        f.write("".join(f"{w:08x}\n" for w in words))
    base = ["vvp", "-n", vvp, f"+hex={hexf}", f"+maxcycles={maxcycles}"]
    # run 1: implementation alone -> expectations
    r = subprocess.run(base + ["+nospec"] + (["+trace"] if trace else []),
                       capture_output=True, text=True, timeout=120)
    out = r.stdout
    final = parse_final(out)
    problems = []
    # run 2: lock-step co-simulation with the spec -> first mismatch
    mism = []
    if not nospec:
        r2 = subprocess.run(base, capture_output=True, text=True, timeout=120)
        mism = [l for l in r2.stdout.splitlines() if l.startswith("MISMATCH")]
    cosim_ok = not mism
    for l in mism:
        m = re.match(r"MISMATCH (\d+) pc=(\w+) insn=(\w+) (\w+) impl=(\w+) spec=(\w+)", l)
        if not m:
            problems.append(l)
            continue
        n, pc, insn, field, iv, sv = m.groups()
        where = (f"co-sim mismatch after commit #{n} (pc={pc}: "
                 f"{rvasm.disasm(int(insn, 16), int(pc, 16))})")
        iv, sv = int(iv, 16), int(sv, 16)
        if field in ("regs", "dmem"):
            nm = "x{}" if field == "regs" else "mem{}"
            for k in range(4):
                a, b = (iv >> (32 * k)) & 0xFFFFFFFF, (sv >> (32 * k)) & 0xFFFFFFFF
                if a != b:
                    problems.append(f"{where}: {nm.format(k)} impl={a:08x} spec={b:08x}")
        else:
            problems.append(f"{where}: {field} impl={iv:x} spec={sv:x}")
    expect_ok = True
    if final is None:
        expect_ok = False
        problems.append("simulation produced no FINAL line:\n" + out + r.stderr)
    else:
        if not final.get("halted"):
            expect_ok = False
            problems.append(f"did not reach 'halt' within {maxcycles} cycles")
        for k, v in expectations(prog).items():
            if k not in final:
                problems.append(f"unknown expectation key {k}")
                expect_ok = False
            elif final[k] != v:
                problems.append(f"expected {k}=0x{v:x}, implementation has 0x{final[k]:x}")
                expect_ok = False
    return {"ok": expect_ok and (cosim_ok or nospec), "expect_ok": expect_ok,
            "cosim_ok": cosim_ok, "final": final, "problems": problems, "out": out}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--which", default="src", help="directory with spec_core.v (src or solutions)")
    ap.add_argument("--defects", default="", help="comma list: glitch,backdoor")
    ap.add_argument("--mutant", type=int, default=0)
    ap.add_argument("--defs", default=os.path.join(LAB, "build"), help="directory with defects.vh")
    ap.add_argument("--out", default=os.path.join(LAB, "build", "sim"))
    ap.add_argument("--nospec", action="store_true", help="do not co-simulate the spec")
    ap.add_argument("--trace", action="store_true", help="print every commit")
    ap.add_argument("programs", nargs="*")
    a = ap.parse_args()
    which = a.which if os.path.isabs(a.which) else os.path.join(LAB, a.which)
    defects = [d for d in a.defects.replace(" ", ",").split(",") if d]
    progs = a.programs or sorted(glob.glob(os.path.join(LAB, "programs", "*.s")))
    try:
        vvp = build(which, a.defs, a.out, defects, a.mutant)
    except RuntimeError as e:
        print(e)
        return 2
    label = f"spec={os.path.relpath(which, LAB)}" + (f" defects={','.join(defects)}" if defects else "") + \
        (f" mutant={a.mutant}" if a.mutant else "")
    print(f"== make sim ({label})")
    allok = True
    for p in progs:
        try:
            r = run_program(vvp, p, a.out, a.nospec, a.trace)
        except rvasm.AsmError as e:
            print(f"  {os.path.basename(p):12s} ASSEMBLY ERROR: {e}")
            allok = False
            continue
        if a.trace:
            for l in r["out"].splitlines():
                m = re.match(r"COMMIT (\d+) pc=(\w+) insn=(\w+)", l)
                if m:
                    pc, w = int(m.group(2), 16), int(m.group(3), 16)
                    print(f"      #{m.group(1):>4} {pc:08x}: {w:08x}  {rvasm.disasm(w, pc)}")
        exp = "ok" if r["expect_ok"] else "FAIL"
        cos = "skipped" if a.nospec else ("ok" if r["cosim_ok"] else "MISMATCH")
        cyc = r["final"]["cycles"] if r["final"] else "?"
        print(f"  {os.path.basename(p):12s} expectations: {exp:4s}  co-sim vs spec: {cos:8s} ({cyc} cycles)")
        for msg in r["problems"]:
            print(f"      - {msg}")
        allok &= r["ok"]
    print("== all programs passed" if allok else "== some programs FAILED")
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
