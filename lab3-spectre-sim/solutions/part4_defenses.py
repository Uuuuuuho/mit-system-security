"""Part 4 -- Defenses (REFERENCE SOLUTION: spoilers!).

Two software defenses, written as assembly-to-assembly compiler passes that are
applied to the kernel (and, for the overhead measurement, to the benchmarks),
and two hardware defenses, written as speculation policies for the core.
"""

from specsim import Policy, asmtools


# ---------------------------------------------------------------------------
# Exercise 4-1: fence after every conditional branch
# ---------------------------------------------------------------------------
def fence_pass(src):
    """Insert a `fence` at the start of both successors of every conditional
    branch: right after the branch (fall-through) and right after each label
    that is the target of a conditional branch."""
    lines = asmtools.parse(src)
    targets = {ln.target for ln in lines if ln.is_cond_branch}
    out = []
    for ln in lines:
        out.append(ln)
        if ln.is_cond_branch or (ln.is_label and ln.label in targets):
            out.append("    fence")
    return asmtools.render(out)


# ---------------------------------------------------------------------------
# Exercise 4-2: speculative load hardening (SLH)
# ---------------------------------------------------------------------------
PRED, TMP = "tp", "gp"      # registers reserved for the pass


def slh_pass(src):
    """LLVM-style speculative load hardening.

    * PRED (tp) is all-ones on the correct path and becomes 0 as soon as the
      program runs down a mispredicted edge.  It is reset at every .entry.
    * On each edge of a conditional branch, PRED &= (edge condition really
      holds ? -1 : 0), computed branch-free from the branch operands.  The taken
      edge gets its own trampoline block so the update applies to that edge only.
    * Every loaded value is ANDed with PRED, so data loaded on a mispredicted
      path is zeroed before anything can use it.  Because PRED depends on the
      branch operands, the AND also waits until the branch inputs are known.
    """
    lines = asmtools.parse(src)
    out, tramps = [], []
    for ln in lines:
        if ln.op == ".entry":
            out += [ln, f"    li {PRED}, -1"]
            continue
        if ln.is_cond_branch:
            rs1, rs2, target = ln.args
            tl = asmtools.new_label("slh")
            out.append(f"    {ln.op} {rs1}, {rs2}, {tl}")
            # fall-through edge: the branch must really be not-taken (TMP == 0)
            out += asmtools.branch_taken(ln.op, rs1, rs2, TMP)
            out += [f"    addi {TMP}, {TMP}, -1",            # 0 -> -1, 1 -> 0
                    f"    and {PRED}, {PRED}, {TMP}"]
            # taken edge: the branch must really be taken (TMP == 1)
            tramps.append(f"{tl}:")
            tramps += asmtools.branch_taken(ln.op, rs1, rs2, TMP)
            tramps += [f"    sub {TMP}, zero, {TMP}",          # 1 -> -1, 0 -> 0
                       f"    and {PRED}, {PRED}, {TMP}",
                       f"    j {target}"]
            continue
        out.append(ln)
        if ln.is_load:
            rd = ln.args[0]
            out.append(f"    and {rd}, {rd}, {PRED}")
    if tramps:
        out += [".text"] + tramps
    return asmtools.render(out)


# ---------------------------------------------------------------------------
# Exercise 4-3: InvisiSpec-style invisible speculative loads
# ---------------------------------------------------------------------------
class InvisiSpecPolicy(Policy):
    """Speculative loads read the cache hierarchy without changing it; the line
    is only filled when the load retires (i.e. turned out to be on the correct
    path).  Squashed loads leave no trace."""

    name = "invisispec"

    def on_load(self, ev, caches):
        if ev.spec:
            ev.expose = True
            return caches.peek(ev.addr)
        return caches.access(ev.addr)


# ---------------------------------------------------------------------------
# Exercise 4-4: STT-style taint tracking
# ---------------------------------------------------------------------------
class STTPolicy(Policy):
    """Speculative Taint Tracking: the value returned by a speculative load is
    tainted until the load reaches its visibility point.  Taint flows through
    ALU instructions (done by the core).  A load whose address is tainted (a
    "transmitter") may not issue before its address becomes untainted."""

    name = "stt"
    track_taint = True

    def load_taint(self, ev):
        return ev.vis if ev.spec else 0

    def transmit_delay(self, ev, addr_taint):
        return max(ev.t, addr_taint)
