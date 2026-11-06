#!/usr/bin/env python3
"""smoke.py - end-to-end smoke test of psp-lite and its SDK (run via `make smoke`).

Checks: the CLI, ELF and flat loading, the ISA self-test (golden), traps and
privilege, serial I/O, the flag CSRs, ASLR + self-relocation, the shadow-stack
hook (stub vs. reference implementation), unhandled-trap and instruction-limit
exit codes, the trace format, and simulator speed (>= 20 MIPS).
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f"  ({detail})" if detail and not ok else ""))


def run(cmd, stdin=b"", timeout=60):
    p = subprocess.run(cmd, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    return p.returncode, p.stdout.decode(errors="replace"), p.stderr.decode(errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    a = ap.parse_args()
    iss = os.path.join(a.build, "psp-lite")
    ssref = os.path.join(a.build, "ss-ref", "psp-lite")
    ex = lambda n: os.path.join(a.build, "examples", n)

    rc, out, _ = run([iss, "--version"])
    check("--version", rc == 0 and out.startswith("psp-lite"), out)

    for img in ("hello.elf", "hello.bin"):
        rc, out, err = run([iss, "--golden", ex(img)], b"abc\n")
        check(f"hello ({img}): serial I/O and exit code", rc == 7 and "you typed 3 bytes: abc" in out, f"rc={rc} {out!r}")

    for args in ([], ["--aslr"]):
        for img in ("isa_test.elf", "isa_test.bin"):
            rc, out, err = run([iss, "--golden", *args, ex(img)])
            check(f"isa_test {img} {' '.join(args)}", rc == 0 and "149/149 passed" in out, out + err)

    rc, out, err = run([iss, "--flag", "FLAG{smoke-test}", ex("trap_demo.elf")])
    check("trap_demo: U-mode, ecall, CSR privilege, custom-0 trap",
          rc == 0 and "3 traps handled, last mcause=2" in out and "FLAG{smoke-test}" in out, out + err)

    bases = []
    for s in ("1", "2", "3"):
        rc, out, err = run([iss, "--aslr-seed", s, ex("aslr_demo.elf")])
        m = re.search(r"load base \(CSR 0xCC0\)\s+= 0x([0-9a-f]+)", out)
        ok = rc == 0 and m and "names[2]=gamma ops -> 7" in out
        bases.append(int(m.group(1), 16) if m else None)
        check(f"aslr_demo --aslr-seed {s}: relocated and working", bool(ok), out + err)
    check("ASLR bases differ and are 4 KiB aligned",
          None not in bases and len(set(bases)) == 3 and all(b % 4096 == 0 and b >= 0x80000000 for b in bases), str(bases))
    rc, out, _ = run([iss, ex("aslr_demo.bin")])
    check("no --aslr: image at 0x80000000", "load base (CSR 0xCC0)  = 0x80000000" in out, out)

    rc, out, _ = run([iss, ex("ss_demo.elf")])
    check("ss_demo without shadow stack: late crash at garbage address", rc >= 64 and rc != 82 and "returned normally" not in out, f"rc={rc}")
    rc, out, _ = run([iss, "--shadow-stack", ex("ss_demo.elf")])
    check("ss_demo with the stub hook: same late crash", rc >= 64 and rc != 82, f"rc={rc}")
    rc, out, err = run([ssref, "--shadow-stack", ex("ss_demo.elf")])
    check("ss_demo with reference shadow stack: trap 18", rc == 82 and "shadow stack violation" in err,
          f"rc={rc} {err}")
    for img in ("isa_test.elf", "aslr_demo.elf", "trap_demo.elf"):
        rc, out, err = run([ssref, "--shadow-stack", "--aslr", ex(img)])
        check(f"reference shadow stack: no false alarm on {img}", rc == 0, f"rc={rc} {err}")

    with tempfile.TemporaryDirectory() as td:
        bad = os.path.join(td, "bad.bin")
        open(bad, "wb").write(bytes.fromhex("ffffffff"))
        rc, _, err = run([iss, bad])
        check("unhandled illegal instruction -> exit 66", rc == 66 and "mcause=2" in err, f"rc={rc} {err}")
        loop = os.path.join(td, "loop.bin")
        open(loop, "wb").write((0x0000006F).to_bytes(4, "little"))  # j .
        rc, _, err = run([iss, "--max-instr", "1000", loop])
        check("instruction limit -> exit 124", rc == 124, f"rc={rc}")
        tr = os.path.join(td, "trace.txt")
        rc, _, _ = run([iss, "--golden", "--trace-file", tr, ex("hello.elf")], b"x\n")
        lines = open(tr).read().splitlines()
        ok = len(lines) > 100 and re.match(r"\s+0 M 80000000 [0-9a-f]{8}  auipc\s+t0, 0x0\s+t0=80000000$", lines[0])
        check("trace format", bool(ok), lines[0] if lines else "empty")
        rc, _, err = run([iss, "--golden", "--dump-regs", ex("hello.elf")], b"")
        check("--dump-regs", "x10 a0" in err and "pc=" in err, err[:200])

    rc, out, err = run([iss, "--golden", "--stats", ex("bench.elf")], timeout=120)
    m = re.search(r"([0-9.]+) MIPS", err)
    mips = float(m.group(1)) if m else 0.0
    check(f"speed >= 20 MIPS (measured {mips:.0f})", rc == 0 and mips >= 20.0, err)

    n = sum(results)
    print(f"smoke: {n}/{len(results)} checks passed")
    sys.exit(0 if n == len(results) else 1)


if __name__ == "__main__":
    main()
