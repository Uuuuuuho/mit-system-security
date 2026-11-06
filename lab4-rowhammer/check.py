#!/usr/bin/env python3
"""Grader for lab4-rowhammer.

    python3 check.py [--bin build] [--seed N] [--parts "1 2 3"] [-q]

Runs the harness program of every part (build/partN) with a per-run seed, prints its output and
a summary, and exits 0 only if every selected part passes. `make check` / `make check-solution`
call this script. Part 1 runs twice: with the simulated pagemap (always) and in auto mode
(native /proc/self/pagemap when PFNs are visible, otherwise the simulated fallback).
"""
import argparse
import os
import re
import subprocess
import sys
import time

PARTS = {
    1: "virt <-> phys translation (pagemap)",
    2: "bank conflicts by timing",
    3: "bank XOR function recovery",
    4: "hammering: single/double/many-sided, TRR",
    5: "SECDED Hamming(39,32)",
    6: "mitigations: PARA and Graphene",
}
RESULT_RE = re.compile(r"RESULT part=(\d+) status=(PASS|FAIL) passed=(\d+) total=(\d+)")
TIMEOUT_S = 150


def run_one(binary, args, quiet):
    t0 = time.time()
    if not os.path.exists(binary):
        return False, 0, 1, f"missing {binary} (did it compile? run `make`)", 0.0, ""
    try:
        p = subprocess.run([binary] + args, capture_output=True, text=True, timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return False, 0, 1, f"timed out after {TIMEOUT_S} s", time.time() - t0, ""
    dt = time.time() - t0
    out = p.stdout
    if p.stderr.strip():
        out += "".join("  stderr: " + line + "\n" for line in p.stderr.strip().splitlines()[-8:])
    if not quiet:
        sys.stdout.write("".join("  | " + line + "\n" for line in out.rstrip().splitlines()))
    m = RESULT_RE.search(p.stdout)
    if not m:
        why = f"no RESULT line (exit code {p.returncode}"
        why += ", crashed)" if p.returncode < 0 else ")"
        if quiet:
            sys.stdout.write("".join("  | " + line + "\n" for line in out.rstrip().splitlines()[-10:]))
        return False, 0, 1, why, dt, out
    if quiet:
        for line in out.splitlines():
            if "[FAIL]" in line:
                print("  | " + line)
    ok = m.group(2) == "PASS"
    return ok, int(m.group(3)), int(m.group(4)), "", dt, out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default="build", help="directory with the part binaries")
    ap.add_argument("--seed", type=int, default=None, help="base seed (default: $SEED or the clock)")
    ap.add_argument("--parts", default="1 2 3 4 5 6", help='e.g. "2 3"')
    ap.add_argument("--label", default="your code", help="what is being graded (for the banner)")
    ap.add_argument("-q", "--quiet", action="store_true", help="only print failures and the summary")
    a = ap.parse_args()

    seed = a.seed
    if seed is None:
        seed = int(os.environ["SEED"]) if os.environ.get("SEED") else int(time.time()) % 1000000007
    parts = sorted({int(x) for x in a.parts.replace(",", " ").split()} & set(PARTS))
    print(f"lab4-rowhammer grader: grading {a.label} in {a.bin}/  (seed {seed}; reproduce with SEED={seed})")

    summary = []
    t_all = time.time()
    for part in parts:
        binary = os.path.join(a.bin, f"part{part}")
        runs = [("sim pagemap", ["--mode", "sim"]), ("auto pagemap", ["--mode", "auto"])] if part == 1 else [("", [])]
        part_ok, got, tot, dt_part, notes = True, 0, 0, 0.0, []
        for label, extra in runs:
            print(f"\n=== Part {part}: {PARTS[part]}" + (f" [{label}]" if label else ""))
            ok, p, t, why, dt, out = run_one(binary, ["--seed", str(seed + part)] + extra, a.quiet)
            if part == 1 and extra[-1] == "auto":
                m = re.search(r"^BACKEND (\w+)", out, re.M)
                if m:
                    notes.append(f"auto backend = {m.group(1)}")
            if why:
                print(f"  -> FAIL: {why}")
                notes.append(why)
            part_ok &= ok
            got += p
            tot += t
            dt_part += dt
        summary.append((part, part_ok, got, tot, dt_part, notes))

    print("\n" + "=" * 78)
    print(f"Summary ({a.label}, seed {seed})")
    for part, ok, got, tot, dt, notes in summary:
        extra = ("  [" + "; ".join(notes) + "]") if notes else ""
        print(f"  Part {part}  {PARTS[part]:<42} {'PASS' if ok else 'FAIL'}  {got:>2}/{tot:<2} checks  {dt:5.1f}s{extra}")
    nparts = sum(1 for s in summary if s[1])
    ch_got = sum(s[2] for s in summary)
    ch_tot = sum(s[3] for s in summary)
    print(f"Score: {nparts}/{len(summary)} parts, {ch_got}/{ch_tot} checks  ({time.time() - t_all:.1f}s)")
    return 0 if summary and nparts == len(summary) else 1


if __name__ == "__main__":
    sys.exit(main())
