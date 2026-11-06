"""Hardware defense hooks ("speculation policies").

The core calls a Policy object for every load it executes, on the correct path
and on mispredicted (transient) paths alike.  The base class is the unprotected
baseline machine.  Part 4 asks you to write two subclasses (InvisiSpec-like and
STT-like) in src/part4_defenses.py.

A policy only sees what real hardware could know when the load issues:

    ev.addr    physical address of the load
    ev.t       cycle at which the load issues (its address operand is ready)
    ev.spec    True if some older branch is still unresolved at ev.t, i.e. the load
               is speculative and might later be squashed
    ev.vis     the "visibility point": the cycle at which every older branch has
               resolved, so the load can no longer be squashed. For a load on a
               path that will be squashed, vis is INF (it never becomes safe).
    ev.kernel  True if the load executes in kernel mode
    ev.expose  output flag, see on_load()

Note that ev.spec does NOT tell you whether the load is on the wrong path: the
hardware cannot know that until the branch resolves.

Cache access methods available to on_load() (the `caches` argument):
    caches.access(addr) -> latency   normal access: fills L1/L2, updates LRU
    caches.peek(addr)   -> latency   lookup only: no fill, no LRU change

Register taint (only if `track_taint = True`): the core keeps, for every
register, a cycle number "taint" = the time until which the value must be treated
as speculative/secret.  Untainted values have taint 0.  ALU instructions give
their result the max of their source taints.  Loads set the taint of their result
with load_taint(), and before a load issues the core calls transmit_delay() with
the taint of its *address* register to find the earliest cycle it may issue.
"""

INF = 1 << 60


class LoadEvent:
    __slots__ = ("addr", "t", "spec", "vis", "kernel", "expose")

    def __init__(self, addr, t, spec, vis, kernel):
        self.addr = addr
        self.t = t
        self.spec = spec
        self.vis = vis
        self.kernel = kernel
        self.expose = False

    def __repr__(self):
        return (f"LoadEvent(addr={self.addr:#x}, t={self.t}, spec={self.spec}, "
                f"vis={'INF' if self.vis >= INF else self.vis}, kernel={self.kernel})")


class Policy:
    """Baseline: no defense.  Speculative loads behave exactly like normal loads."""

    name = "baseline"
    track_taint = False

    def on_load(self, ev, caches):
        """Perform the cache side of a load and return its latency in cycles.

        Set ev.expose = True to ask the core to fill the line into the cache when
        (and only if) the load retires, i.e. once it is known to be on the correct
        path.  Exposed fills happen at the load's retirement cycle."""
        return caches.access(ev.addr)

    def load_taint(self, ev):
        """Taint (a cycle number) for the value this load returns.  0 = untainted."""
        return 0

    def transmit_delay(self, ev, addr_taint):
        """Earliest cycle at which this load may issue, given the taint of its
        address operand.  Return ev.t to issue without delay."""
        return ev.t
