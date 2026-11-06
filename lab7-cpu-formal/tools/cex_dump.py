#!/usr/bin/env python3
"""Print a counterexample VCD (from `make bmc`) as a readable instruction trace.

    cex_dump.py [build/bmc/cex.vcd] [--all]

The trace is reconstructed from the port signals of the two cores (impl.* and
spec.*), so it works with any miter that instantiates them as `impl` and
`spec` (as the skeleton does).  For each time step it shows what the
implementation does (reset / FETCH / DECODE / EXEC / WB=commit), the
instruction being executed (disassembled), the operands the ALU saw, and the
architectural state that changed.  At every instruction boundary it compares
the implementation with the spec and prints the fields that differ.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rvasm  # noqa: E402

NREGS, DMEM_WORDS = 4, 4
FSM = {0: "FETCH", 1: "DECODE", 2: "EXEC", 3: "WB"}


# ------------------------------------------------------------- VCD reader --
def read_vcd(path):
    """Return (names{id: name}, widths{name: w}, steps[list of {name: int|None}])."""
    ids, widths = {}, {}
    events = []            # (time, id, value-string)
    t = 0
    in_defs = True
    with open(path) as f:
        data = f.read()
    tokens = data.split()
    i = 0
    scope = []
    while i < len(tokens):
        tok = tokens[i]
        if in_defs:
            if tok == "$scope":
                scope.append(tokens[i + 2])
                i += 4
                continue
            if tok == "$upscope":
                if scope:
                    scope.pop()
                i += 2
                continue
            if tok == "$var":
                # $var <type> <width> <id> <name...> $end
                j = i + 4
                name_parts = []
                while tokens[j] != "$end":
                    name_parts.append(tokens[j])
                    j += 1
                name = "".join(name_parts).lstrip("\\")
                # keep names relative to the top scope
                if len(scope) > 1:
                    name = ".".join(s.lstrip("\\") for s in scope[1:]) + "." + name
                w = int(tokens[i + 2])
                ids.setdefault(tokens[i + 3], []).append(name)
                widths[name] = w
                i = j + 1
                continue
            if tok == "$enddefinitions":
                in_defs = False
                i += 2
                continue
            if tok.startswith("$"):
                # skip $date/$version/$comment/$timescale blocks
                j = i + 1
                while j < len(tokens) and tokens[j] != "$end":
                    j += 1
                i = j + 1
                continue
            i += 1
            continue
        if tok in ("$dumpvars", "$end", "$dumpall", "$dumpon", "$dumpoff"):
            i += 1
            continue
        if tok.startswith("#"):
            t = int(tok[1:])
            i += 1
            continue
        if tok[0] in "bBrR":
            events.append((t, tokens[i + 1], tok[1:]))
            i += 2
            continue
        if tok[0] in "01xXzZ":
            events.append((t, tok[1:], tok[0]))
            i += 1
            continue
        i += 1

    def conv(s):
        s = s.lower()
        if any(c in "xz" for c in s):
            return None
        return int(s, 2) if s else 0

    # Decide the sampling convention.  yosys `sim` (ABC engine) toggles clk:
    # sample right after each rising edge (and at time 0).  yosys `sat`
    # (sat engine) never toggles clk: time t is step max(t, 1).
    clk_ids = [k for k, v in ids.items() if "clk" in v]
    toggles = [e for e in events if e[1] in clk_ids and e[2] == "1"]
    cur = {}
    steps = []
    times = sorted(set(e[0] for e in events))
    by_time = {}
    for e in events:
        by_time.setdefault(e[0], []).append(e)
    if toggles:
        rise_times = sorted(set(e[0] for e in toggles))
        sample_at = [0] + [rt for rt in rise_times if rt != 0]
    else:
        sample_at = times
    sample_set = set(sample_at)
    for tm in times:
        for _, vid, val in by_time[tm]:
            v = conv(val)
            for name in ids.get(vid, []):
                cur[name] = v
        if tm in sample_set:
            steps.append(dict(cur))
    return widths, steps


# --------------------------------------------------------------- analysis --
def g(st, name, default=None):
    v = st.get(name)
    return default if v is None else v


def split_words(v, n):
    if v is None:
        return [None] * n
    return [(v >> (32 * k)) & 0xFFFFFFFF for k in range(n)]


def arch(st, who):
    """Architectural state of 'impl' or 'spec' from the core's port signals."""
    pc = g(st, "impl.dbg_pc") if who == "impl" else g(st, "spec.spec_pc")
    return {
        "pc": pc,
        "priv": g(st, f"{who}.dbg_priv"),
        "regs": split_words(g(st, f"{who}.dbg_regs"), NREGS),
        "mtvec": g(st, f"{who}.dbg_mtvec"),
        "mepc": g(st, f"{who}.dbg_mepc"),
        "mcause": g(st, f"{who}.dbg_mcause"),
        "mpp": g(st, f"{who}.dbg_mpp"),
        "dmem": split_words(g(st, f"{who}.dbg_dmem"), DMEM_WORDS),
    }


def diff_arch(a, b):
    """Fields that differ.  Unknown values (x: signals the miter never looks at,
    which yosys optimised away) are not counted as differences."""
    out = []

    def cmp(name, u, v):
        if u is not None and v is not None and u != v:
            out.append((name, u, v))
    for k in ("pc", "priv", "mtvec", "mepc", "mcause", "mpp"):
        cmp(k, a[k], b[k])
    for r in range(NREGS):
        cmp(f"x{r}", a["regs"][r], b["regs"][r])
    for r in range(DMEM_WORDS):
        cmp(f"mem[{r}]", a["dmem"][r], b["dmem"][r])
    return out


def fmt(v, w=8):
    return "x" * w if v is None else f"{v:0{w}x}"


def analyse(steps):
    """Turn the per-step signal values into a list of event dicts."""
    evs = []
    cur_insn = None
    cur_pc = None
    for n, st in enumerate(steps, 1):
        e = {"step": n, "rst": bool(g(st, "impl.rst", 0)), "state": g(st, "impl.state")}
        if e["rst"]:
            e["what"] = "reset"
            evs.append(e)
            continue
        fsm = FSM.get(e["state"], "?")
        e["fsm"] = fsm
        if g(st, "impl.imem_en"):
            e["fetch_pc"] = g(st, "impl.imem_addr")
            e["fetch_insn"] = g(st, "impl.imem_rdata")
            cur_insn, cur_pc = e["fetch_insn"], e["fetch_pc"]
            ia, sa = arch(st, "impl"), arch(st, "spec")
            e["boundary_diff"] = diff_arch(ia, sa)
            e["impl_arch"] = ia
        if fsm == "EXEC":
            e["alu_a"] = g(st, "impl.alu_a")
            e["alu_b"] = g(st, "impl.alu_b")
            e["alu_res"] = g(st, "impl.eres")
        if g(st, "impl.commit"):
            e["commit_pc"] = g(st, "impl.commit_pc")
            e["commit_insn"] = g(st, "impl.commit_insn")
            e["spec_insn"] = g(st, "spec.insn")
            e["spec_pc"] = g(st, "spec.spec_pc")
            e["spec_step"] = g(st, "spec.step")
        e["insn"], e["pc"] = cur_insn, cur_pc
        evs.append(e)
    # state changes between consecutive steps (impl view)
    prev = None
    for e, st in zip(evs, steps):
        a = arch(st, "impl")
        if prev is not None and not e["rst"] and not evs[e["step"] - 2]["rst"]:
            e["changes"] = diff_arch(prev, a)
        prev = a
    return evs


def failing_step(evs):
    """First step at which a standard miter check fails (or None)."""
    for e in evs:
        if e.get("boundary_diff"):
            return e["step"], "state mismatch at instruction boundary"
        if "commit_insn" in e:
            if e.get("spec_insn") is not None and e["commit_insn"] != e["spec_insn"]:
                return e["step"], "committed instruction != instruction given to the spec"
            if e.get("spec_pc") is not None and e["commit_pc"] != e["spec_pc"]:
                return e["step"], "commit pc != spec pc"
    return None, None


def render(evs, show_all=False, header=None):
    lines = []
    if header:
        lines.append(header)
    fs, why = failing_step(evs)
    lines.append(f"{'step':>4}  {'impl':6s}  event")
    for e in evs:
        n = e["step"]
        if e["rst"]:
            lines.append(f"{n:4d}  {'reset':6s}")
            continue
        fsm = e.get("fsm", "?")
        parts = []
        if "fetch_pc" in e:
            bd = e.get("boundary_diff")
            chk = "boundary check: " + ("MISMATCH" if bd else "ok")
            w = e["fetch_insn"]
            parts.append(f"{chk:24s} fetch pc={fmt(e['fetch_pc'])} insn={fmt(w)}  "
                         f"{rvasm.disasm(w, e['fetch_pc']) if w is not None else '?'}")
        if "alu_a" in e and (show_all or e["insn"] is not None):
            w = e["insn"]
            dis = rvasm.disasm(w).split()[0] if w is not None else "?"
            if dis in ("addi", "add", "xor", "srli", "lui", "lw", "sw"):
                parts.append(f"ALU({dis}): a={fmt(e['alu_a'])} b={fmt(e['alu_b'])} -> {fmt(e['alu_res'])}")
        if "commit_pc" in e:
            w = e["commit_insn"]
            parts.append(f"COMMIT pc={fmt(e['commit_pc'])}  {rvasm.disasm(w, e['commit_pc']) if w is not None else '?'}")
        ch = e.get("changes") or []
        ch = [c for c in ch if c[0] != "pc" or show_all]
        if ch:
            parts.append("  ".join(f"{k}: {fmt(a)}->{fmt(b)}" for k, a, b in ch))
        if not parts and not show_all:
            parts.append("")
        lines.append(f"{n:4d}  {fsm:6s}  " + ("\n" + " " * 14).join(parts))
        if fs == n:
            lines.append(" " * 14 + f"*** {why} ***")
            if e.get("boundary_diff"):
                lines.append(" " * 18 + f"{'field':8s} {'impl':>10s} {'spec':>10s}")
                for k, a, b in e["boundary_diff"]:
                    lines.append(" " * 18 + f"{k:8s} {fmt(a):>10s} {fmt(b):>10s}")
            break
    if fs is None:
        lines.append("(no implementation/spec difference visible at instruction boundaries or "
                     "commits; the failing assertion is probably the liveness/other check)")
    return "\n".join(lines)


def summary(evs):
    """Machine-readable digest used by the grader."""
    fs, why = failing_step(evs)
    commits = [e for e in evs if "commit_pc" in e and (fs is None or e["step"] <= fs)]
    execs = [e for e in evs if "alu_a" in e and (fs is None or e["step"] <= fs)]
    return {"fail_step": fs, "reason": why,
            "commits": [{"step": e["step"], "pc": e["commit_pc"], "insn": e["commit_insn"]}
                        for e in commits],
            "execs": [{"step": e["step"], "insn": e["insn"], "a": e["alu_a"], "b": e["alu_b"]}
                      for e in execs],
            "last_boundary_diff": next((e["boundary_diff"] for e in evs if e.get("boundary_diff")), [])}


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    path = args[0] if args else os.path.join(os.path.dirname(HERE), "build", "bmc", "cex.vcd")
    if not os.path.exists(path):
        print(f"cex_dump: {path} not found -- run `make bmc` first (it only writes a "
              f"counterexample when the check FAILS)")
        return 1
    _, steps = read_vcd(path)
    evs = analyse(steps)
    if "--json" in argv:
        print(json.dumps(summary(evs), indent=1))
        return 0
    hdr = None
    meta = os.path.join(os.path.dirname(path), "result.json")
    if os.path.exists(meta):
        try:
            m = json.load(open(meta))
            hdr = (f"Counterexample from `make bmc` ({m.get('engine')}, K={m.get('k')}, "
                   f"spec={m.get('which')}, defines: {' '.join(m.get('defines', [])) or '-'})\n"
                   f"Failing step reported by the solver: {m.get('step')}   (step 1 = reset)")
            if m.get("failed_asserts"):
                hdr += "\nAssertions that fail in the replay:\n" + "\n".join(
                    "  " + a for a in m["failed_asserts"])
            hdr += "\n"
        except (ValueError, OSError):
            hdr = None
    print(render(evs, "--all" in argv, hdr))
    print(f"\n(full waveform: {path} -- open it with GTKWave or any VCD viewer)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
