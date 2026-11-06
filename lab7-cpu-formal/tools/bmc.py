#!/usr/bin/env python3
"""Bounded model checking of the psp-mini miter with yosys.

    bmc.py [--which src|solutions] [--k K] [--defects glitch,backdoor]
           [--mutant N] [--alu-only] [--nosync] [--engine abc|sat]

Flow (all tools are part of the yosys package):

  1. yosys: read_verilog -formal (psp_mini.v + spec_core.v + miter.v), prep,
     flatten, memory_map, give every flip-flop the initial value 0.
  2a. engine=abc (default): lower to an and-inverter graph and write AIGER
     (asserts -> "bad state" outputs, assumes -> constraints), then run ABC's
     bounded model checker `bmc3 -F K` (yosys-abc).  If a counterexample
     exists, ABC writes a witness (.aiw) which yosys `sim` replays on the
     word-level design to produce cex.vcd.
  2b. engine=sat: yosys' built-in SAT-based BMC
         sat -tempinduct-baseonly -maxsteps K -prove-asserts -set-assumes
             -set-init-zero -show-public -dump_vcd cex.vcd
     (same question, simpler solver: fine for a few instructions, but it
     stalls once the model needs more than ~2 instructions; see README).

Step numbering: step 1 is the reset cycle, so with psp-mini's 4 cycles per
instruction the n-th instruction commits in step 4n+1 and is checked at the
next instruction boundary, step 4n+2.

Exit status: 0 = no counterexample up to K steps, 1 = counterexample found,
2 = tool error / no assertions / timeout.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LAB = os.path.dirname(HERE)
sys.path.insert(0, HERE)


def defines_for(defects, mutant, alu_only, nosync, extra=()):
    d = []
    if alu_only:
        d.append("ALU_ONLY")
    if nosync:
        d.append("NOSYNC")
    if "glitch" in defects:
        d.append("PSP_GLITCH")
    if "backdoor" in defects:
        d.append("PSP_BACKDOOR")
    if mutant:
        d.append(f"PSP_MUTANT={mutant}")
    d.extend(extra)
    return d


def run(cmd, timeout, log):
    t0 = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        out = r.stdout + r.stderr
        rc = r.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
        out += "\n*** TIMEOUT ***\n"
        rc = -1
    with open(log, "a") as f:
        f.write(f"$ {' '.join(cmd)}\n{out}\n")
    return rc, out, time.time() - t0


def bmc(which="src", k=14, defects=(), mutant=0, alu_only=False, nosync=False, engine="abc",
        defs_dir=None, out_dir=None, timeout=900, extra_defines=(), quiet=False):
    """Run one BMC job.  Returns a result dict (also written to out_dir/result.json)."""
    which_dir = which if os.path.isabs(which) else os.path.join(LAB, which)
    defs_dir = defs_dir or os.path.join(LAB, "build")
    out_dir = out_dir or os.path.join(LAB, "build", "bmc")
    os.makedirs(out_dir, exist_ok=True)
    for f in ("cex.vcd", "cex.aiw", "result.json", "bmc.log"):
        p = os.path.join(out_dir, f)
        if os.path.exists(p):
            os.remove(p)
    log = os.path.join(out_dir, "bmc.log")
    defs = defines_for(defects, mutant, alu_only, nosync, extra_defines)
    res = {"which": os.path.relpath(which_dir, LAB), "k": k, "engine": engine, "defines": defs,
           "status": "error", "step": None, "seconds": 0.0, "failed_asserts": [], "message": ""}
    t0 = time.time()
    if not os.path.exists(os.path.join(defs_dir, "defects.vh")):
        res["message"] = f"{defs_dir}/defects.vh missing (run `make` first)"
        return finish(res, out_dir, t0, quiet)

    srcs = [os.path.join(LAB, "rtl", "psp_mini.v"), os.path.join(which_dir, "spec_core.v"),
            os.path.join(which_dir, "miter.v")]
    rd = "read_verilog -formal " + " ".join("-D" + d for d in defs) + \
         f" -I{os.path.join(LAB, 'rtl')} -I{defs_dir} " + " ".join(srcs)
    front = [rd, "prep -top miter", "flatten", "memory_map", "opt -fast",
             f"tee -q -o {out_dir}/cells.txt select -count t:$assert"]

    if engine == "sat":
        ys = front + [
            f"sat -tempinduct-baseonly -maxsteps {k} -prove-asserts -set-assumes -set-init-zero "
            f"-show-public -dump_vcd {out_dir}/cex.vcd"]
    else:
        ys = front + [
            "setundef -zero -init",
            f"write_rtlil {out_dir}/design.il",
            "techmap", "opt_clean", "dffunmap", "aigmap", "opt_clean",
            f"write_aiger -zinit -map {out_dir}/design.aim {out_dir}/design.aig"]
    ysf = os.path.join(out_dir, "bmc.ys")
    with open(ysf, "w") as f:
        f.write("\n".join(ys) + "\n")

    rc, out, _ = run(["yosys", "-q", "-l", os.path.join(out_dir, "yosys.log"), ysf], timeout, log)
    nasserts = None
    try:
        m = re.search(r"(\d+) objects", open(os.path.join(out_dir, "cells.txt")).read())
        nasserts = int(m.group(1)) if m else None
    except OSError:
        pass
    if nasserts == 0:
        res["message"] = "the miter contains no assertions (nothing to check) -- write them first"
        return finish(res, out_dir, t0, quiet)

    if engine == "sat":
        if "*** TIMEOUT ***" in out:
            res["message"] = f"timeout after {timeout}s"
            return finish(res, out_dir, t0, quiet)
        ylog = open(os.path.join(out_dir, "yosys.log")).read()
        if "SUCCESS!" in ylog:
            res["status"] = "pass"
        elif "FAIL!" in ylog:
            res["status"] = "fail"
            cases = re.findall(r"\[base case (\d+)\]", ylog)
            res["step"] = int(cases[-1]) if cases else None
        else:
            res["message"] = "yosys failed:\n" + tail(out)
        return finish(res, out_dir, t0, quiet)

    if rc != 0:
        res["message"] = "yosys failed:\n" + tail(out)
        return finish(res, out_dir, t0, quiet)

    abc = shutil.which("yosys-abc") or "yosys-abc"
    script = (f"read_aiger {out_dir}/design.aig; fold; strash; bmc3 -F {k} -v; "
              f"write_cex -a {out_dir}/cex.aiw")
    rc, out, _ = run([abc, "-c", script], timeout, log)
    if "*** TIMEOUT ***" in out:
        res["message"] = f"timeout after {timeout}s"
        return finish(res, out_dir, t0, quiet)
    m = re.search(r"Output (\d+) of miter .* was asserted in frame (\d+)", out)
    if m:
        res["status"] = "fail"
        res["step"] = int(m.group(2)) + 1
        # replay the witness on the word-level design to get a readable VCD
        rc2, out2, _ = run(["yosys", "-q", "-p",
                            f"read_rtlil {out_dir}/design.il; sim -clock clk -r {out_dir}/cex.aiw "
                            f"-map {out_dir}/design.aim -vcd {out_dir}/cex.vcd"], timeout, log)
        res["failed_asserts"] = describe_asserts(out2)
    elif re.search(r"No output asserted in (\d+) frames", out):
        res["status"] = "pass"
    else:
        res["message"] = "ABC failed:\n" + tail(out)
    return finish(res, out_dir, t0, quiet)


def describe_asserts(sim_out):
    """'Warning: Assert miter.$assert$src/miter.v:114$436 (...) failed.' -> source lines."""
    seen, out = set(), []
    # yosys' source range for an assert statement may start at the end of the
    # previous statement; the statement itself is on the range's last line.
    for m in re.finditer(r"Assert \S*?\$assert\$([^:$]+):(\d+)\$\d+ \(([^)]*)\) failed", sim_out):
        f, ln = m.group(1), int(m.group(2))
        rng = re.search(r":(\d+)\.\d+-(\d+)\.\d+$", m.group(3))
        if rng:
            ln = int(rng.group(2))
        if (f, ln) in seen:
            continue
        seen.add((f, ln))
        text = ""
        try:
            path = f if os.path.isabs(f) else os.path.join(LAB, f)
            text = open(path).read().splitlines()[ln - 1].strip()
        except (OSError, IndexError):
            pass
        out.append(f"{os.path.relpath(f, LAB) if os.path.isabs(f) else f}:{ln}: {text}")
    return out


def tail(s, n=25):
    return "\n".join(s.strip().splitlines()[-n:])


def finish(res, out_dir, t0, quiet):
    res["seconds"] = round(time.time() - t0, 1)
    with open(os.path.join(out_dir, "result.json"), "w") as f:
        json.dump(res, f, indent=1)
    if not quiet:
        print_result(res, out_dir)
    return res


def print_result(res, out_dir):
    d = " ".join(res["defines"]) or "-"
    print(f"BMC  spec={res['which']}  K={res['k']}  engine={res['engine']}  defines: {d}")
    if res["status"] == "pass":
        print(f"PASS: no counterexample in {res['k']} steps "
              f"(= reset + {(res['k'] - 2) // 4} complete, checked instructions)   [{res['seconds']} s]")
    elif res["status"] == "fail":
        print(f"FAIL: counterexample of length {res['step']} steps   [{res['seconds']} s]")
        for a in res["failed_asserts"]:
            print(f"   failing assertion  {a}")
        vcd = os.path.join(out_dir, "cex.vcd")
        if os.path.exists(vcd):
            import cex_dump
            _, steps = cex_dump.read_vcd(vcd)
            evs = cex_dump.analyse(steps)
            fs, why = cex_dump.failing_step(evs)
            commits = [e for e in evs if "commit_pc" in e]
            print(f"   waveform: {os.path.relpath(vcd)}   readable trace: make cex")
            if commits:
                print("   instructions in the trace:")
                for e in commits:
                    w = e["commit_insn"]
                    print(f"     step {e['step']:3d}  pc={e['commit_pc']:08x}  {w:08x}  "
                          f"{cex_dump.rvasm.disasm(w, e['commit_pc'])}")
            if why:
                print(f"   first visible difference: step {fs}: {why}")
                for k, a, b in cex_dump.summary(evs)["last_boundary_diff"]:
                    print(f"       {k:8s} impl={cex_dump.fmt(a)}  spec={cex_dump.fmt(b)}")
    else:
        print(f"ERROR: {res['message']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--which", default="src")
    ap.add_argument("--k", type=int, default=14)
    ap.add_argument("--defects", default="")
    ap.add_argument("--mutant", type=int, default=0)
    ap.add_argument("--alu-only", action="store_true")
    ap.add_argument("--nosync", action="store_true")
    ap.add_argument("--engine", choices=["abc", "sat"], default="abc")
    ap.add_argument("--defs", default=None)
    ap.add_argument("--out", default=None)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("-D", dest="extra", action="append", default=[], help="extra Verilog define")
    a = ap.parse_args()
    defects = [d for d in a.defects.replace(" ", ",").split(",") if d]
    bad = [d for d in defects if d not in ("glitch", "backdoor")]
    if bad:
        print(f"unknown defect(s) {bad}; use glitch and/or backdoor")
        return 2
    r = bmc(a.which, a.k, defects, a.mutant, a.alu_only, a.nosync, a.engine, a.defs, a.out,
            a.timeout, a.extra)
    return {"pass": 0, "fail": 1}.get(r["status"], 2)


if __name__ == "__main__":
    sys.exit(main())
