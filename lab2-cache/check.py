#!/usr/bin/env python3
"""Grader for lab2-cache.

    python3 check.py                 grade your code (bin/partN), sim backend
    python3 check.py --solution      grade the reference solutions (build/solution/bin/partN)
    python3 check.py --parts 2,3     only some parts
    python3 check.py --seed 1234     reproduce a run (each trial uses a seed derived from this one)
    python3 check.py --backend native  try the native backend (informational; not used by make check)
    python3 check.py -v              show the output of every trial

Each part runs several trials with fresh random secrets and needs a minimum number of passes.
Before running anything, the grader inspects your object files with `nm`: attack code must not call
the harness (hx_*) or simulator internals, and the Part 5 sender/receiver must not share symbols or
use files, pipes, shared memory, signals, environment variables or process tricks.
Exit status is 0 only if every part passes.
"""
import argparse
import os
import random
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# (key, title, binary, extra args, trials, needed, points, objects checked)
PARTS = [
    ("1", "Part 1  latency histograms + thresholds", "part1", [], 5, 4, 10, ["part1/calibrate.o"]),
    ("2", "Part 2  Flush+Reload CTF", "part2", [], 5, 4, 15, ["part2/flush_reload.o"]),
    ("3a", "Part 3a Prime+Probe CTF, 16 lines", "part3", ["--mode", "easy"], 5, 4, 15,
     ["part3/evset.o", "part3/prime_probe.o"]),
    ("3b", "Part 3b Prime+Probe CTF, 4 lines, bursty", "part3", ["--mode", "hard"], 5, 4, 10,
     ["part3/evset.o", "part3/prime_probe.o"]),
    ("4a", "Part 4a 2-bit key from code paths", "part4", ["--bits", "2"], 5, 4, 15, ["part4/key_leak.o"]),
    ("4b", "Part 4b constant-trace lookup (defense)", "part4", ["--ct-check"], 3, 3, 10,
     ["part4/victim_lookup.o"]),
    ("5", "Part 5  covert dead drop", "part5", [], 5, 4, 25, ["part5/sender.o", "part5/receiver.o"]),
]

FORBIDDEN_PREFIXES = ("hx_", "sim_", "hier_", "cache_", "rng_")
FORBIDDEN_EXACT = {"sim_backend", "native_backend"}
# Part 5: ways to talk that are not the cache
P5_FORBIDDEN = {
    "open", "open64", "openat", "openat64", "__open_2", "__open64_2", "fopen", "fopen64", "freopen", "creat",
    "mmap", "mmap64", "shm_open", "shmget", "shmat", "pipe", "pipe2", "socket", "socketpair", "mkfifo",
    "mkfifoat", "msgget", "msgsnd", "msgrcv", "semget", "memfd_create", "getenv", "secure_getenv", "setenv",
    "putenv", "unsetenv", "kill", "sigqueue", "raise", "signal", "sigaction", "ptrace", "process_vm_readv",
    "process_vm_writev", "fork", "vfork", "clone", "syscall", "dlopen", "dlsym", "system", "popen", "execve",
    "execv", "execvp", "execl", "execlp",
}


def nm_symbols(obj):
    """Return (undefined, defined_global) symbol name sets of an object file, or None if nm is missing."""
    if not shutil.which("nm"):
        return None
    und = subprocess.run(["nm", "-u", obj], capture_output=True, text=True).stdout.split()
    und = {s for s in und if s not in ("U", "w", "v")}
    dfn = set()
    for line in subprocess.run(["nm", "-g", "--defined-only", obj], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            dfn.add(parts[2])
    return und, dfn


def static_checks(build_dir, objs, part_key):
    """List of problems found in the given object files (empty = fine)."""
    problems = []
    syms = {}
    for rel in objs:
        path = os.path.join(build_dir, rel)
        if not os.path.exists(path):
            problems.append(f"{rel}: object file missing (did it compile?)")
            continue
        r = nm_symbols(path)
        if r is None:
            return []  # no nm: skip silently (the README says so)
        syms[rel] = r
        und, _ = r
        bad = sorted(s for s in und if s.startswith(FORBIDDEN_PREFIXES) or s in FORBIDDEN_EXACT)
        if bad:
            problems.append(f"{rel}: uses harness/simulator internals: {', '.join(bad)}")
        if part_key == "5":
            bad = sorted(s for s in und if s in P5_FORBIDDEN)
            if bad:
                problems.append(f"{rel}: Part 5 must talk only through the cache, but uses: {', '.join(bad)}")
    if part_key == "5" and len(syms) == 2:
        (a, (ua, da)), (b, (ub, db)) = syms.items()
        shared = sorted((ua & db) | (ub & da))
        if shared:
            problems.append(f"sender and receiver share symbols: {', '.join(shared)}")
    return problems


def run_trial(binary, args, seed, backend, timeout):
    env = dict(os.environ)
    env["SHD_BACKEND"] = backend
    env.pop("SEED", None)
    t0 = time.time()
    try:
        p = subprocess.run([binary, "--seed", str(seed)] + args, capture_output=True, text=True, timeout=timeout,
                           env=env, cwd=HERE)
        out = p.stdout + p.stderr
        code = p.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        out += "\n[grader] killed after %d s\n" % timeout
        code = None
    dt = time.time() - t0
    verdict = "C"  # crash / no verdict
    for line in out.splitlines()[::-1]:
        if line.startswith("RESULT "):
            if line.startswith("RESULT PASS"):
                verdict = "."
            elif "timeout" in line:
                verdict = "T"
            else:
                verdict = "F"
            break
    if code is None:
        verdict = "T"
    return verdict, out, dt


def main():
    ap = argparse.ArgumentParser(description="lab2-cache grader")
    ap.add_argument("--solution", action="store_true", help="grade the reference solutions")
    ap.add_argument("--parts", default="", help="comma-separated part numbers, e.g. 2,3")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--trials", type=int, default=None, help="override the number of trials per part")
    ap.add_argument("--backend", default="sim", choices=["sim", "native"])
    ap.add_argument("--timeout", type=int, default=150, help="seconds per trial")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    tree = "solution" if a.solution else "student"
    bindir = os.path.join(HERE, "build", "solution", "bin") if a.solution else os.path.join(HERE, "bin")
    objdir = os.path.join(HERE, "build", tree)
    base = a.seed if a.seed is not None else random.randrange(1, 2**31)
    want = {p.strip() for p in a.parts.split(",") if p.strip()}

    print(f"lab2-cache grader: tree={tree} backend={a.backend} seed={base}   (reproduce: make check{'-solution' if a.solution else ''} SEED={base})")
    total = got = 0
    all_ok = True
    t_start = time.time()
    for idx, (key, title, binname, args, trials, need, points, objs) in enumerate(PARTS):
        if want and key not in want and key.rstrip("ab") not in want:
            continue
        total += points
        if a.trials:
            trials, need = a.trials, max(1, (a.trials * need + trials - 1) // trials)
        binary = os.path.join(bindir, binname)
        if not os.path.exists(binary):
            print(f"  {title:44s} FAIL  (missing {os.path.relpath(binary, HERE)}; run make)")
            all_ok = False
            continue
        problems = static_checks(objdir, objs, key.rstrip("ab"))
        if problems:
            print(f"  {title:44s} FAIL  (rule check)")
            for pr in problems:
                print(f"      - {pr}")
            all_ok = False
            continue
        backend = "sim" if key == "4b" else a.backend  # the trace check needs the simulator
        marks, first_bad, secs = "", None, 0.0
        for t in range(trials):
            seed = (base + 7919 * (idx + 1) + 104729 * t) % (2**31 - 1) + 1
            v, out, dt = run_trial(binary, args, seed, backend, a.timeout)
            marks += v
            secs += dt
            if a.verbose:
                print(f"----- {title} trial {t + 1} seed={seed} -> {v} ({dt:.1f}s)\n{out.rstrip()}\n")
            if v != "." and first_bad is None:
                first_bad = (seed, out)
        npass = marks.count(".")
        ok = npass >= need
        if ok:
            got += points
        else:
            all_ok = False
        print(f"  {title:44s} [{marks:5s}] {npass}/{trials} {'PASS' if ok else 'FAIL'}  ({secs:5.1f}s)")
        if not ok and first_bad and not a.verbose:
            seed, out = first_bad
            tail = [l for l in out.splitlines() if l.strip()][-4:]
            print(f"      first failing trial (seed {seed}): ./{os.path.relpath(binary, HERE)} --seed {seed} {' '.join(args)}")
            for l in tail:
                print(f"        | {l}")
    print(f"legend: . pass  F wrong answer  T timeout  C crash/no verdict      total time {time.time() - t_start:.1f}s")
    print(f"Score: {got}/{total}")
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
