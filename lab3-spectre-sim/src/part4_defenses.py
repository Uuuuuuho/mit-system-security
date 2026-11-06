"""Part 4 -- Defenses.

See README.md, Part 4.  Two software defenses, written as assembly-to-assembly
compiler passes (str -> str), and two hardware defenses, written as speculation
policies for the core.  The grader applies each one, runs YOUR Part 2 attack
against the defended machine, and measures the slowdown on the benchmark suite
(specsim/bench.py).  The software passes are applied to the kernel and to the
benchmarks; the policies change the core.

Rules for the passes: the registers gp and tp are reserved for you (kernel and
benchmarks never use them).  Programs mark their entry points with `.entry`.
The transformed program must compute exactly the same results.
"""

from specsim import Policy, asmtools


# ---------------------------------------------------------------------------
# Exercise 4-1: fences
# ---------------------------------------------------------------------------
def fence_pass(src):
    """Stop speculation past every conditional branch by inserting `fence` at the
    start of both of its successors (fall-through and target)."""
    lines = asmtools.parse(src)
    out = []
    for ln in lines:
        out.append(ln)
        # TODO
    return asmtools.render(out)


# ---------------------------------------------------------------------------
# Exercise 4-2: speculative load hardening
# ---------------------------------------------------------------------------
def slh_pass(src):
    """Speculative load hardening (Carruth, LLVM 2018), adapted to this ISA:

    * keep a predicate register (tp) that is all-ones on the correct path and
      becomes 0 on a mispredicted path; set it to -1 at every `.entry`;
    * on each edge of every conditional branch, AND into tp a mask computed
      *without branches* from the branch operands: -1 if the edge's condition
      really holds, 0 otherwise.  asmtools.branch_taken(op, rs1, rs2, "gp")
      gives you gp = 1 if taken else 0.  The taken edge needs its own little
      block (a trampoline: update tp, then jump to the original target) --
      asmtools.new_label() makes fresh labels; put trampolines after a `.text`
      line at the end of the output;
    * harden every loaded value: after each load, `and rd, rd, tp`.
    """
    lines = asmtools.parse(src)
    out = []
    for ln in lines:
        out.append(ln)
        # TODO
    return asmtools.render(out)


# ---------------------------------------------------------------------------
# Exercise 4-3: InvisiSpec-style invisible speculation
# ---------------------------------------------------------------------------
class InvisiSpecPolicy(Policy):
    """Speculative loads must not change the cache.  They may read it (and
    memory) to get their data; the line is installed only once the load is
    known to be on the correct path (at retirement).  Non-speculative loads
    behave normally.  See specsim/policy.py for ev fields and cache methods."""

    name = "invisispec"

    def on_load(self, ev, caches):
        # TODO
        return caches.access(ev.addr)


# ---------------------------------------------------------------------------
# Exercise 4-4: STT-style speculative taint tracking
# ---------------------------------------------------------------------------
class STTPolicy(Policy):
    """Data returned by a speculative load is tainted until that load can no
    longer be squashed (its visibility point).  Taint propagates through ALU
    instructions (the core does that).  A load whose *address* is tainted is a
    transmitter: it must wait until the taint is gone.  Untainted loads,
    including speculative ones, are not delayed."""

    name = "stt"
    track_taint = True

    def load_taint(self, ev):
        # TODO: return the cycle until which this load's result is tainted (0 = never)
        return 0

    def transmit_delay(self, ev, addr_taint):
        # TODO: return the earliest cycle this load may issue
        return ev.t
