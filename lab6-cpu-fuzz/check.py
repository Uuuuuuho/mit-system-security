#!/usr/bin/env python3
"""check.py - grader for lab6-cpu-fuzz.

Builds a psp-lite CPU with a FRESH random seed (so hard-coded answers fail), asks
the grader-only defects dump for the ground truth, builds your programs (or the
reference solutions with --solution) and grades:

  Part 1 (15)  sp.c reports a genuinely faulty addition and still prints the
               golden answer (checksum identical to a --golden run)
  Part 2 (25)  addscan.c finds exactly the faulty pairs in [0x100, 0x500)^2
  Part 3 (30)  scan.c lists exactly the undocumented non-trapping custom-0 encoding
  Part 4 (30)  fuzz.py finds the hidden decode defect by differential fuzzing

Usage:  python3 check.py [--solution] [--seed N] [--part 1|2|3|4]
"""
import argparse
import json
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PSP = os.path.normpath(os.path.join(HERE, "..", "platform", "psp-lite"))
LO, HI = 0x100, 0x500


def sh(cmd, timeout=600, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, **kw)


def faulty(a, b, t):
    """Ground-truth model of the seeded adder defect (operands both in [0x100, 0xFFFF])."""
    if (a | b) >> 16 or not (a >> 8) or not (b >> 8):
        return False
    pair = (a & 15, b & 15)
    return pair in ((t["nibble_a"], t["nibble_b"]), (t["nibble_b"], t["nibble_a"]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--solution", action="store_true", help="grade solutions/ instead of your code")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--part", default="all")
    a = ap.parse_args()
    seed = a.seed if a.seed is not None else random.randrange(1, 1 << 31)
    src = "solutions" if a.solution else "."
    tag = "solutions" if a.solution else "student"
    root = os.path.join(HERE, "build", f"check-{tag}-{seed}")
    cpu_dir = os.path.join(root, "cpu")
    print(f"== lab6 grader: {'reference solutions' if a.solution else 'your code'}, fresh CPU seed {seed}")

    r = sh(["make", "-s", "-C", PSP, f"BUILD={cpu_dir}", f"SEED={seed}", "DEFECTS=1", "all", "defects-dump"])
    if r.returncode:
        print(r.stdout + r.stderr)
        sys.exit("could not build psp-lite")
    cpu = os.path.join(cpu_dir, "psp-lite")
    truth = json.loads(sh([os.path.join(cpu_dir, "psp-defects")]).stdout)
    out = os.path.join(root, "prog")
    r = sh(["make", "-s", "-C", HERE, f"SRC={src}", f"OUT={out}", f"SEED={seed}", f"CPU_DIR={cpu_dir}", "programs"])
    build_ok = r.returncode == 0
    if not build_ok:
        print("program build failed:\n" + (r.stdout + r.stderr)[-3000:])

    want = {"1", "2", "3", "4"} if a.part == "all" else {a.part}
    score, total, results = 0, 0, []

    def grade(part, pts, ok, why):
        nonlocal score, total
        total += pts
        score += pts if ok else 0
        results.append((part, pts, ok, why))
        print(f"[{'PASS' if ok else 'FAIL'}] Part {part} ({pts} pts): {why}")

    def run(elf, *extra, timeout=120):
        try:
            return sh([cpu, "--max-instr", "400000000", *extra, elf], timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    if "1" in want:
        elf = os.path.join(out, "part1", "sp.elf")
        ok, why = False, "not built"
        if os.path.exists(elf):
            d, g = run(elf), run(elf, "--golden")
            if d and g:
                bad = re.findall(r"bad_add a=0x([0-9a-f]+) b=0x([0-9a-f]+) got=0x([0-9a-f]+) want=0x([0-9a-f]+)", d.stdout)
                ck_d = re.search(r"checksum = (\w+)", d.stdout)
                ck_g = re.search(r"checksum = (\w+)", g.stdout)
                genuine = [x for x in bad if faulty(int(x[0], 16), int(x[1], 16), truth["add"])
                           and int(x[3], 16) == (int(x[0], 16) + int(x[1], 16)) & 0xFFFFFFFF
                           and int(x[2], 16) == int(x[3], 16) ^ (1 << truth["add"]["bit"])]
                if not bad:
                    why = "no 'bad_add a=.. b=.. got=.. want=..' line on the defective CPU"
                elif len(genuine) != len(bad):
                    why = f"reported additions that are not actually faulty: {bad[:2]}"
                elif not (ck_d and ck_g and ck_d.group(1) == ck_g.group(1)):
                    why = "final distances differ from the --golden run (continue with the correct sum)"
                else:
                    ok, why = True, f"{len(bad)} genuine faulty addition(s) reported, answer matches golden"
            else:
                why = "program timed out"
        grade("1", 15, ok, why)

    if "2" in want:
        elf = os.path.join(out, "part2", "addscan.elf")
        ok, why = False, "not built"
        if os.path.exists(elf):
            d = run(elf, timeout=300)
            exp = [(x, y) for x in range(LO, HI) for y in range(LO, HI) if faulty(x, y, truth["add"])]
            if d is None:
                why = "timed out"
            else:
                m = re.search(r"count=(\d+)", d.stdout)
                it = re.search(r"iterations=(\d+)", d.stdout)
                fm = re.search(r"first a=0x([0-9a-f]+) b=0x([0-9a-f]+)", d.stdout)
                pm = re.search(r"pattern nibble_a=(\d+) nibble_b=(\d+) bit=(\d+)", d.stdout)
                if not (m and it):
                    why = "missing count=/iterations= lines"
                elif int(it.group(1)) != (HI - LO) ** 2:
                    why = f"iterations={it.group(1)}, expected {(HI - LO) ** 2} (loop control disturbed?)"
                elif int(m.group(1)) != len(exp):
                    why = f"count={m.group(1)}, expected {len(exp)}"
                elif exp and not (fm and (int(fm.group(1), 16), int(fm.group(2), 16)) == exp[0]):
                    why = f"first faulty pair should be a=0x{exp[0][0]:x} b=0x{exp[0][1]:x}"
                elif exp and not (pm and int(pm.group(3)) == truth["add"]["bit"]
                                  and {int(pm.group(1)), int(pm.group(2))} == {truth["add"]["nibble_a"], truth["add"]["nibble_b"]}):
                    why = "pattern line wrong"
                else:
                    ok, why = True, f"count={len(exp)}, pattern correct"
        grade("2", 25, ok, why)

    if "3" in want:
        elf = os.path.join(out, "part3", "scan.elf")
        ok, why = False, "not built"
        if os.path.exists(elf):
            d = run(elf, timeout=120)
            b = truth["backdoor"]
            exp = (b["funct7"] << 25) | (b["funct3"] << 12) | 0x0B
            if d is None:
                why = "timed out"
            else:
                got = re.findall(r"undocumented=0x([0-9a-f]{8})", d.stdout)
                if [int(x, 16) for x in got] == [exp]:
                    ok, why = True, f"undocumented=0x{exp:08x}"
                elif "undocumented=none" in d.stdout:
                    why = "reported none (is the trap handler finished?)"
                else:
                    why = f"reported {got}, expected exactly one encoding"
                if d.returncode == 124:
                    why += " [hit the instruction limit: a trap loops?]"
        grade("3", 30, ok, why)

    if "4" in want:
        fz = os.path.join(HERE, src, "part4", "fuzz.py")
        ok, why = False, "fuzz.py missing"
        if os.path.exists(fz):
            try:
                d = sh([sys.executable, fz, "--cpu", cpu, "--tests", "600", "--time-budget", "100",
                        "--seed", str(seed)], timeout=200)
                found = re.findall(r"^FOUND (\w+)", d.stdout, re.M)
                mn = truth["decode"]["mnemonic"]
                if mn in found:
                    ok, why = True, f"found the decode defect in '{mn}' (all findings: {sorted(set(found))})"
                else:
                    why = f"decode defect not found (findings: {sorted(set(found)) or 'none'})"
            except subprocess.TimeoutExpired:
                why = "fuzzer timed out"
        grade("4", 30, ok, why)

    print(f"== score {score}/{total}")
    sys.exit(0 if score == total and build_ok else 1)


if __name__ == "__main__":
    main()
