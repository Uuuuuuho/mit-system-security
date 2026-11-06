#!/usr/bin/env python3
"""Grader for lab7-cpu-formal.

    check.py [--which src|solutions] [--jobs N] [--seed S]

Grades a spec_core.v + miter.v pair (default: your files in src/).  Every run
places the two planted defects with a fresh random seed (or SEED=/--seed), so
a counterexample cannot be hard-coded.

  Part 1  ALU spec + miter      co-sim alu.s; BMC (ALU-only, no defects) passes;
                                mutants 1-4 are caught
  Part 2  the ALU glitch        BMC (ALU-only, glitch on) finds a counterexample
                                and the trace really triggers the glitch
  Part 3  full ISA + backdoor   co-sim all programs; BMC (no defects) passes;
                                BMC (backdoor on) finds a counterexample that
                                executes the backdoor; mutants 5-14 are caught
  Part 4  bounded proof         BMC with both defects off passes at K=30

Exit status 0 only if every part passes.
"""
import argparse
import concurrent.futures as cf
import glob
import os
import random
import subprocess
import sys
import time

LAB = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(LAB, "tools"))
import bmc as bmcmod  # noqa: E402
import cex_dump  # noqa: E402
import gen_defects  # noqa: E402
import sim as simmod  # noqa: E402

K_ALU, K_FULL, K_FIND, K_PROOF = 18, 14, 18, 30

MUTANTS = {
    1: "XOR computes OR",
    2: "SRLI shifts arithmetically",
    3: "ADDI immediate not sign-extended",
    4: "x0 is writable",
    5: "BEQ compares only 16 bits",
    6: "LW ignores its offset",
    7: "trap saves pc+4 in mepc",
    8: "MRET does not clear MPP",
    9: "mtvec writable from U-mode",
    10: "ECALL from U reports cause 11",
    11: "JAL links pc instead of pc+4",
    12: "custom-0 executes as a NOP",
    13: "MRET hangs the pipeline",
    14: "commit port: wrong pc for BEQ",
}


def job_list():
    jobs = [("p1-proof", dict(k=K_ALU, alu_only=True), "pass")]
    jobs += [(f"p1-m{m}", dict(k=K_ALU, alu_only=True, mutant=m), "fail") for m in range(1, 5)]
    jobs += [("p2-glitch", dict(k=K_FIND, alu_only=True, defects=["glitch"]), "fail")]
    jobs += [("p3-proof", dict(k=K_FULL), "pass"),
             ("p3-backdoor", dict(k=K_FIND, defects=["backdoor"]), "fail")]
    jobs += [(f"p3-m{m}", dict(k=K_FIND, mutant=m), "fail") for m in range(5, 15)]
    jobs += [("p4-proof", dict(k=K_PROOF), "pass")]
    return jobs


def glitch_witness(vcd, d):
    """Does the trace contain an ADD/ADDI with the magic operands?"""
    _, steps = cex_dump.read_vcd(vcd)
    evs = cex_dump.analyse(steps)
    fs, _ = cex_dump.failing_step(evs)
    for e in evs:
        if "alu_a" not in e or e["insn"] is None or (fs is not None and e["step"] > fs):
            continue
        w = e["insn"]
        is_addi = (w & 0x707F) == 0x0013
        is_add = (w & 0xFE00707F) == 0x0033
        if (is_add or is_addi) and e["alu_a"] == d["glitch_a"] and \
                e["alu_b"] is not None and (e["alu_b"] & 0xFF) == d["glitch_b"]:
            return True, f"{cex_dump.rvasm.disasm(w)} with a=0x{e['alu_a']:08x}, b[7:0]=0x{d['glitch_b']:02x}"
    return False, "the trace never feeds the magic operands to the adder"


def backdoor_witness(vcd, d):
    """Does the trace commit the backdoor encoding with the key in rs1?"""
    _, steps = cex_dump.read_vcd(vcd)
    evs = cex_dump.analyse(steps)
    fs, _ = cex_dump.failing_step(evs)
    by_step = {e["step"]: e for e in evs}
    for e in evs:
        if "commit_insn" not in e or (fs is not None and e["step"] > fs):
            continue
        w = e["commit_insn"]
        if w is None or (w & 0x7F) != 0x0B:
            continue
        if ((w >> 12) & 7) != d["bd_funct3"] or (w >> 25) != d["bd_funct7"]:
            continue
        b = by_step.get(e["step"] - 3, {})
        regs = b.get("impl_arch", {}).get("regs")
        r = (w >> 15) & 3
        if regs and r != 0 and regs[r] == d["bd_key"]:
            return True, f"custom-0 word 0x{w:08x} with x{r}=0x{regs[r]:08x}"
    return False, "the trace never executes the backdoor encoding with the key in rs1"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--which", default="src")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("JOBS", "2")))
    ap.add_argument("--seed", default=os.environ.get("SEED", ""))
    a = ap.parse_args()
    which = os.path.join(LAB, a.which) if not os.path.isabs(a.which) else a.which
    seed = int(a.seed) if a.seed else random.SystemRandom().randrange(1, 2**31)
    wtag = os.path.basename(os.path.normpath(which))
    gdir = os.path.join(LAB, "build", f"grade-{wtag}")
    defs = os.path.join(gdir, "defs")
    subprocess.run([sys.executable, os.path.join(LAB, "tools", "gen_defects.py"), defs, str(seed)],
                   check=True, capture_output=True)
    dj = gen_defects.values(seed)
    t0 = time.time()
    print(f"lab7-cpu-formal grader: spec + miter from {wtag}/, defects seed {seed}, "
          f"{a.jobs} parallel job(s)\n")

    # ---------------------------------------------------------------- sims --
    simres = {}
    sim_err = None
    try:
        vvp = simmod.build(which, defs, os.path.join(gdir, "sim"))
        for p in sorted(glob.glob(os.path.join(LAB, "programs", "*.s"))):
            simres[os.path.basename(p)] = simmod.run_program(vvp, p, os.path.join(gdir, "sim"))
    except Exception as e:  # compile error in the student's spec etc.
        sim_err = str(e)

    # ---------------------------------------------------------------- BMC --
    jobs = job_list()
    results = {}

    def work(j):
        name, kw, _ = j
        return name, bmcmod.bmc(which=which, defs_dir=defs, out_dir=os.path.join(gdir, name),
                                quiet=True, timeout=300, **kw)

    with cf.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        for name, r in ex.map(work, jobs):
            results[name] = r

    # ------------------------------------------------------------ verdicts --
    parts = []

    def bmc_line(name, expect, desc):
        r = results[name]
        if r["status"] == "error":
            return False, f"{desc:44s} ERROR: {r['message'].splitlines()[0] if r['message'] else '?'}"
        if expect == "pass":
            ok = r["status"] == "pass"
            txt = f"no counterexample (K={r['k']})" if ok else \
                f"UNEXPECTED counterexample at step {r['step']} (see {os.path.relpath(gdir)}/{name}/)"
        else:
            ok = r["status"] == "fail"
            txt = f"caught at step {r['step']}" if ok else f"NOT caught within K={r['k']}"
        return ok, f"{desc:44s} {'ok  ' if ok else 'FAIL'}  {txt}  [{r['seconds']} s]"

    def sim_line(prog):
        if sim_err:
            return False, f"co-sim {prog:10s} ERROR: {sim_err.splitlines()[0]}"
        r = simres[prog]
        ok = r["cosim_ok"] and r["expect_ok"]
        why = "" if ok else ("  " + (r["problems"][0] if r["problems"] else ""))
        return ok, f"{'co-sim ' + prog:44s} {'ok  ' if ok else 'FAIL'}{why}"

    # Part 1
    lines = [sim_line("alu.s"), bmc_line("p1-proof", "pass", f"BMC ALU-only, no defects, K={K_ALU}")]
    lines += [bmc_line(f"p1-m{m}", "fail", f"mutant {m:2d}: {MUTANTS[m]}") for m in range(1, 5)]
    parts.append(("Part 1", "ALU spec + miter", 25, lines))

    # Part 2
    r = results["p2-glitch"]
    l2 = [bmc_line("p2-glitch", "fail", f"BMC ALU-only, glitch on, K={K_FIND}")]
    if r["status"] == "fail":
        ok, why = glitch_witness(os.path.join(gdir, "p2-glitch", "cex.vcd"), dj)
        l2.append((ok, f"{'trace triggers the planted glitch':44s} {'ok  ' if ok else 'FAIL'}  {why}"))
    l2.append((results["p1-proof"]["status"] == "pass",
               f"{'same K is clean with the glitch off':44s} "
               f"{'ok  ' if results['p1-proof']['status'] == 'pass' else 'FAIL'}"))
    parts.append(("Part 2", "find the ALU glitch", 20, l2))

    # Part 3
    l3 = [sim_line(p) for p in sorted(simres)] if simres else [sim_line("alu.s")]
    l3.append(bmc_line("p3-proof", "pass", f"BMC full ISA, no defects, K={K_FULL}"))
    l3.append(bmc_line("p3-backdoor", "fail", f"BMC full ISA, backdoor on, K={K_FIND}"))
    r = results["p3-backdoor"]
    if r["status"] == "fail":
        ok, why = backdoor_witness(os.path.join(gdir, "p3-backdoor", "cex.vcd"), dj)
        l3.append((ok, f"{'trace executes the planted backdoor':44s} {'ok  ' if ok else 'FAIL'}  {why}"))
    l3 += [bmc_line(f"p3-m{m}", "fail", f"mutant {m:2d}: {MUTANTS[m]}") for m in range(5, 15)]
    parts.append(("Part 3", "full ISA + exceptions + backdoor", 35, l3))

    # Part 4
    l4 = [bmc_line("p4-proof", "pass", f"bounded proof, both defects off, K={K_PROOF}")]
    parts.append(("Part 4", "bounded safety proof", 20, l4))

    score, total, allok = 0, 0, True
    for title, desc, pts, ls in parts:
        ok = all(o for o, _ in ls)
        allok &= ok
        total += pts
        score += pts if ok else 0
        print(f"{title} [{pts:2d} pts] {desc:34s} {'PASS' if ok else 'FAIL'}")
        for _, l in ls:
            print(f"    {l}")
    p4 = results["p4-proof"]
    print(f"\nPart 4: K={K_PROOF} = reset + {(K_PROOF - 2) // 4} instructions, "
          f"{p4['seconds']} s (yosys + ABC bmc3)")
    print(f"Score: {score}/{total}   (grading took {time.time() - t0:.0f} s; artefacts in "
          f"{os.path.relpath(gdir)}/)")
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
