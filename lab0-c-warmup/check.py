#!/usr/bin/env python3
"""check.py - grader for lab0-c-warmup. Run through `make check` / `make check-solution`.

Every check of ./tester is run in up to three modes, each in a fresh process:
  plain     ./tester CHECK_n          -O2 build; the allocation shim catches leaks, double frees
                                      and makes uninitialized heap memory garbage (0xA5)
  asan      ./tester-asan CHECK_n     AddressSanitizer + UndefinedBehaviorSanitizer (+ LeakSanitizer
                                      when it works in this container)
  valgrind  valgrind ./tester CHECK_n memcheck with leak checking (skipped if valgrind is missing)
A check passes only if it passes in every available mode. Exit code 0 only if all checks pass.
"""
import argparse
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def run(cmd, env=None, timeout=60):
    e = dict(os.environ)
    e.update(env or {})
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=e)
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired as ex:
        out = (ex.stdout or b"").decode(errors="replace") if isinstance(ex.stdout, bytes) else (ex.stdout or "")
        return -999, out + f"\n(timeout after {timeout}s)"


def first_diagnostic(out):
    """Pick the most useful line of a failing run for the summary."""
    for pat in (r"ERROR: AddressSanitizer: [^\n]*", r"runtime error: [^\n]*", r"ERROR: LeakSanitizer: [^\n]*",
                r"alloc_shim: [^\n]*", r"(?:requirement|check) failed[^\n]*", r"leak: [^\n]*",
                r"==\d+== (?:Invalid [^\n]*|Conditional jump[^\n]*|Use of uninitialised[^\n]*|"
                r"[^\n]*definitely lost[^\n]*|ERROR SUMMARY: [1-9][^\n]*)", r"free\(\): [^\n]*",
                r"timeout after[^\n]*"):
        m = re.search(pat, out)
        if m:
            return m.group(0).strip()
    lines = [ln for ln in out.strip().splitlines() if ln.strip()]
    return lines[-1][:160] if lines else "(no output)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=".", help="directory containing tester and tester-asan")
    ap.add_argument("--no-valgrind", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true", help="print full output of failing runs")
    a = ap.parse_args()
    d = os.path.abspath(a.dir)
    plain, asan = os.path.join(d, "tester"), os.path.join(d, "tester-asan")
    for b in (plain, asan):
        if not os.access(b, os.X_OK):
            sys.exit(f"check.py: {b} not found (run make)")
    t0 = time.time()

    rc, out = run([plain, "--list"])
    checks = [ln.split()[0] for ln in out.splitlines() if ln.startswith("CHECK_")]

    # --- decide which modes work on this machine ---
    modes = [("plain", [plain], {})]
    asan_env = {"ASAN_OPTIONS": "abort_on_error=0:detect_leaks=1:allocator_may_return_null=1",
                "UBSAN_OPTIONS": "print_stacktrace=1"}
    rc, out = run([asan, "--probe"], asan_env)
    notes = []
    prefix = []
    if rc != 0 and "LeakSanitizer" in out:
        asan_env["ASAN_OPTIONS"] = asan_env["ASAN_OPTIONS"].replace("detect_leaks=1", "detect_leaks=0")
        notes.append("LeakSanitizer unavailable here (ptrace restricted?): leaks are still caught by the shim")
        rc, out = run([asan, "--probe"], asan_env)
    if rc != 0 and shutil.which("setarch"):
        # Some kernels randomize mmap more than older sanitizer runtimes expect.
        prefix = ["setarch", platform.machine(), "-R"]
        rc, out = run(prefix + [asan, "--probe"], asan_env)
        if rc == 0:
            notes.append("ASan needed ASLR disabled (setarch -R)")
    if rc == 0:
        modes.append(("asan", prefix + [asan], asan_env))
    else:
        notes.append(f"ASan build does not run on this machine, asan mode skipped: {first_diagnostic(out)}")
    if not a.no_valgrind and shutil.which("valgrind"):
        vg = ["valgrind", "-q", "--error-exitcode=99", "--leak-check=full",
              "--errors-for-leak-kinds=definite,indirect", "--track-origins=yes"]
        modes.append(("valgrind", vg + [plain], {"SHIM_NOFILL": "1"}))
    else:
        notes.append("valgrind not installed: valgrind mode skipped")

    print(f"lab0-c-warmup check ({os.path.relpath(d, HERE) or '.'}): modes = {', '.join(m[0] for m in modes)}")
    for n in notes:
        print(f"  note: {n}")
    hdr = f"{'check':<9} " + " ".join(f"{m[0]:<9}" for m in modes) + " result"
    print(hdr)
    print("-" * len(hdr))
    passed = 0
    for c in checks:
        cells, ok_all, diags = [], True, []
        for name, cmd, env in modes:
            rc, out = run(cmd + [c], env, timeout=120 if name == "valgrind" else 60)
            ok = rc == 0 and f"{c}: PASS" in out
            cells.append("pass" if ok else "FAIL")
            if not ok:
                ok_all = False
                diag = first_diagnostic(out)
                if -64 < rc < 0:  # killed by a signal
                    diag = f"crashed with {signal.Signals(-rc).name}; {diag}"
                elif rc == 139:  # some wrappers report 128 + SIGSEGV
                    diag = f"crashed with SIGSEGV; {diag}"
                diags.append(f"{name}: {diag}")
                if a.verbose:
                    diags.append(out)
        passed += ok_all
        print(f"{c:<9} " + " ".join(f"{x:<9}" for x in cells) + (" PASS" if ok_all else " FAIL"))
        for dg in diags:
            print(f"            {dg}")
    print(f"\nExercise 0-1: {'PASS' if passed == len(checks) else 'FAIL'}")
    print(f"score: {passed}/{len(checks)}   ({time.time() - t0:.0f}s)")
    sys.exit(0 if passed == len(checks) else 1)


if __name__ == "__main__":
    main()
