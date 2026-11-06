"""The specsim core: a cycle-approximate, out-of-order-ish timing model.

How it works (see the diagram in README.md):

* Instructions are executed *functionally in program order* (an interpreter),
  while a dataflow timing model assigns each one three cycle numbers:

      d  dispatch : in order, up to cfg.width (4) instructions per cycle, and only
                    when a ROB entry is free (the instruction ROB_SIZE places
                    earlier has retired)
      i  issue    : max(d, cycle at which all source registers are ready)
      c  complete : i + latency (ALU 1, mul 4, div 16, loads = cache latency)

  Retirement is in order: retire(k) = max(c(k), retire(k-1)).  Because issue only
  waits for operands, independent instructions overlap (e.g. several cache misses
  are in flight at once) -- this is the "out-of-order-ish" part.

* Conditional branches are predicted at dispatch (see predictor.py) and resolve at
  i+1, i.e. one cycle after their operands are ready.  If the prediction was
  wrong, the core runs the *wrong path* first (`_wrong_path`): it keeps
  dispatching instructions from the predicted target, up to width per cycle, as long as
  (1) the dispatch cycle is before the branch's resolve cycle and (2) a ROB entry
  is free.  A wrong-path instruction executes only if it *issues* before the
  resolve cycle.  Wrong-path work uses a copy of the register file and a private
  store buffer, so the squash restores all architectural state -- but loads that
  issued on the wrong path have already filled the caches, and that is NOT undone.
  After the squash the front-end restarts at resolve + mispredict penalty.

  => The speculation window is "how long the branch's operands take".  A bound
     that misses in the cache gives a window of ~150 cycles; a bound in L1 gives
     a window of a few cycles.

* Serializing instructions: `fence` (younger instructions wait until all older
  ones completed), `rdcycle` (same, and returns that cycle number), `syscall`,
  `sysret`, `halt`.  On a wrong path, any of them ends speculation.

* Privilege: user mode may access only user memory; kernel mode may access all
  mapped memory.  A user load from kernel memory faults architecturally (the
  program is terminated with ProgramFault).  On a wrong path a faulting load is
  suppressed and returns no data (the model is not Meltdown-vulnerable unless
  `cfg.meltdown` is set).  `clflush` from user mode on a kernel line faults.
"""

from collections import deque

from .isa import (ALU_LAST, RR_LAST, LOAD_LAST, STORE_LAST, BRANCH_LAST, JAL, JALR,
                  FENCE, RDCYCLE, CLFLUSH, SYSCALL, SYSRET, HALT, NOP, M64)
from .layout import KERNEL_BASE, USER_BASE, USER_END, UTEXT_BASE, KTEXT_BASE
from .policy import LoadEvent, INF

USER, KERNEL = 0, 1


class ProgramFault(Exception):
    """An architectural fault: the running program is terminated."""

    def __init__(self, msg, pc=None, mode=None, where=None):
        self.pc, self.mode, self.where = pc, mode, where
        full = msg
        if pc is not None:
            full += f"\n    at {'kernel' if mode else 'user'} pc {pc:#x}"
            if where:
                full += f": {where}"
        super().__init__(full)


class RunLimit(Exception):
    """The run exceeded its instruction limit."""


class CPU:
    def __init__(self, cfg, mem, caches, pred, policy=None):
        self.cfg = cfg
        self.mem = mem
        self.caches = caches
        self.pred = pred
        self.policy = policy
        # instruction memory
        self.ucode, self.usrc = [], []      # user text (grows as programs load)
        self.kcode, self.ksrc = [], []      # kernel text
        self.kernel_entry = None
        # architectural state
        self.regs = [0] * 32
        self.mode = USER
        self.epc = 0
        # timing state
        self.ready = [0] * 32       # cycle at which each register's value is ready
        self.taint = [0] * 32       # STT-style taint per register (cycle numbers)
        self.t_disp = 0             # next dispatch cycle
        self.slot = 0               # instructions already dispatched in cycle t_disp
        self.max_c = 0              # latest completion cycle seen so far
        self.last_r = 0             # retire cycle of the youngest dispatched instruction
        self.rob = [0] * cfg.rob    # ring: retire cycles of the last ROB_SIZE instructions
        self.ri = 0
        self.brmax = 0              # latest resolve cycle of any dispatched branch
        self.pending = deque()      # (cycle, addr): fills deferred to retirement (policies)
        self.trace = None           # optional callable(str)
        self.stats = {"instrs": 0, "transient": 0, "transient_loads": 0,
                      "mispredicts": 0, "syscalls": 0}

    # ------------------------------------------------------------------ helpers
    def where(self, pc):
        if pc >= KTEXT_BASE:
            k, src = (pc - KTEXT_BASE) >> 2, self.ksrc
        else:
            k, src = (pc - UTEXT_BASE) >> 2, self.usrc
        if 0 <= k < len(src):
            name, lineno, text = src[k]
            return f"{name}:{lineno}: {text}"
        return "?"

    def _fault(self, msg, pc, mode):
        raise ProgramFault(msg, pc, mode, self.where(pc))

    def _drain(self, t):
        pend, fill = self.pending, self.caches.fill
        while pend and pend[0][0] <= t:
            fill(pend.popleft()[1])

    def _drain_all(self):
        pend, fill = self.pending, self.caches.fill
        while pend:
            fill(pend.popleft()[1])

    def now(self):
        return self.t_disp

    def _tr(self, wrong, d, i, c, mode, pc):
        fmt = lambda x: "      -" if x is None or x >= INF else f"{x:7d}"
        if pc >= KTEXT_BASE:
            mode = KERNEL       # syscall/sysret already switched `mode`; show where it ran
        elif pc >= UTEXT_BASE:
            mode = USER
        self.trace(f"{'~' if wrong else ' '} d={fmt(d)} i={fmt(i)} c={fmt(c)}  "
                   f"{'K' if mode else 'U'} {pc:#010x}  {self.where(pc).split(': ', 1)[-1]}")

    # ------------------------------------------------------------------ main loop
    def run(self, pc, max_instrs=5_000_000):
        """Run from `pc` in the current mode until `halt`; returns the number of
        instructions executed on the correct path."""
        cfg = self.cfg
        regs, ready, taint = self.regs, self.ready, self.taint
        caches, pred, mem = self.caches, self.pred, self.mem
        access, fill = caches.access, caches.fill
        policy = self.policy
        track = bool(policy is not None and policy.track_taint)
        rob = self.rob
        ROB = len(rob)
        ri = self.ri
        t_disp, max_c, last_r, brmax = self.t_disp, self.max_c, self.last_r, self.brmax
        pend = self.pending
        mode = self.mode
        ucode, kcode = self.ucode, self.kcode
        nu, nk = len(ucode), len(kcode)
        penalty, sys_lat = cfg.mispredict_penalty, cfg.syscall_latency
        W = cfg.width
        slot = self.slot
        trace = self.trace
        stats = self.stats
        n = 0
        try:
            while True:
                if n >= max_instrs:
                    raise RunLimit(f"instruction limit ({max_instrs}) reached at pc {pc:#x}")
                n += 1
                # ---------------- fetch
                if pc >= KTEXT_BASE:
                    if not mode:
                        self._fault("user mode jumped into kernel text", pc, mode)
                    k = (pc - KTEXT_BASE) >> 2
                    if k >= nk or pc & 3:
                        self._fault(f"pc {pc:#x} outside kernel text", pc, mode)
                    inst = kcode[k]
                else:
                    k = (pc - UTEXT_BASE) >> 2
                    if k < 0 or k >= nu or pc & 3:
                        self._fault(f"pc {pc:#x} outside user text (missing halt?)", pc, mode)
                    inst = ucode[k]
                op, rd, rs1, rs2, imm, fn, lat = inst
                # ---------------- dispatch: in order, up to W per cycle, needs a ROB entry
                d = t_disp
                x = rob[ri]
                if x > d:
                    d = t_disp = x
                    slot = 0
                slot += 1
                if slot >= W:
                    t_disp = d + 1
                    slot = 0
                npc = pc + 4

                if op <= ALU_LAST:
                    i = ready[rs1]
                    if d > i:
                        i = d
                    if op <= RR_LAST:
                        x = ready[rs2]
                        if x > i:
                            i = x
                        v = fn(regs[rs1], regs[rs2])
                    else:
                        v = fn(regs[rs1], imm)
                    c = i + lat
                    if rd:
                        regs[rd] = v
                        ready[rd] = c
                        if track:
                            t1 = taint[rs1]
                            if op <= RR_LAST and taint[rs2] > t1:
                                t1 = taint[rs2]
                            taint[rd] = t1

                elif op <= LOAD_LAST:
                    i = ready[rs1]
                    if d > i:
                        i = d
                    addr = (regs[rs1] + imm) & M64
                    if addr < KERNEL_BASE or addr + fn > USER_END:
                        self._fault(f"load from unmapped address {addr:#x}", pc, mode)
                    if addr < USER_BASE and not mode:
                        self._fault(f"user-mode load from kernel address {addr:#x}", pc, mode)
                    v = mem[addr] if fn == 1 else int.from_bytes(mem[addr:addr + fn], "little")
                    if policy is None:
                        if pend:
                            self._drain(i)
                        c = i + access(addr, i)
                        if rd:
                            regs[rd] = v
                            ready[rd] = c
                    else:
                        ev = LoadEvent(addr, i, i < brmax, brmax, mode == KERNEL)
                        if track:
                            t2 = policy.transmit_delay(ev, taint[rs1])
                            if t2 > i:
                                i = t2
                                ev.t = i
                                ev.spec = i < brmax
                        if pend:
                            self._drain(i)
                        caches.now = i
                        c = i + policy.on_load(ev, caches)
                        if rd:
                            regs[rd] = v
                            ready[rd] = c
                            if track:
                                taint[rd] = policy.load_taint(ev)
                        if ev.expose:
                            pend.append((c if c > last_r else last_r, addr))

                elif op <= STORE_LAST:
                    i = ready[rs1]
                    if d > i:
                        i = d
                    x = ready[rs2]
                    if x > i:
                        i = x
                    addr = (regs[rs1] + imm) & M64
                    if addr < KERNEL_BASE or addr + fn > USER_END:
                        self._fault(f"store to unmapped address {addr:#x}", pc, mode)
                    if addr < USER_BASE and not mode:
                        self._fault(f"user-mode store to kernel address {addr:#x}", pc, mode)
                    v = regs[rs2]
                    if fn == 1:
                        mem[addr] = v & 0xFF
                    else:
                        mem[addr:addr + fn] = (v & ((1 << (8 * fn)) - 1)).to_bytes(fn, "little")
                    if pend:
                        self._drain(i)
                    fill(addr)                       # write-allocate
                    c = i + 1

                elif op <= BRANCH_LAST:
                    i = ready[rs1]
                    if d > i:
                        i = d
                    x = ready[rs2]
                    if x > i:
                        i = x
                    c = i + 1                        # resolve cycle
                    if c > brmax:
                        brmax = c
                    taken = fn(regs[rs1], regs[rs2])
                    if taken:
                        npc = imm
                    ptaken = pred.predict(pc)
                    if ptaken != taken:
                        # retire bookkeeping for the branch, then explore the wrong path
                        if c > max_c:
                            max_c = c
                        if c > last_r:
                            last_r = c
                        rob[ri] = last_r
                        ri += 1
                        if ri == ROB:
                            ri = 0
                        if trace:
                            self._tr(False, d, i, c, mode, pc)
                        stats["mispredicts"] += 1
                        self._wrong_path(imm if ptaken else pc + 4, mode, t_disp, slot, c,
                                         rob[ri:] + rob[:ri])
                        pred.update(pc, taken)
                        x = c + penalty
                        if x > t_disp:
                            t_disp = x
                            slot = 0
                        pc = npc
                        continue
                    pred.update(pc, taken)

                elif op == JAL:
                    i, c = d, d + 1
                    if rd:
                        regs[rd] = pc + 4
                        ready[rd] = c
                        taint[rd] = 0
                    npc = imm

                elif op == JALR:
                    i = ready[rs1]
                    if d > i:
                        i = d
                    c = i + 1
                    if c > brmax:
                        brmax = c
                    target = (regs[rs1] + imm) & M64
                    if rd:
                        regs[rd] = pc + 4
                        ready[rd] = d + 1
                        taint[rd] = 0
                    npc = target
                    ptarget = pred.btb_predict(pc)
                    pred.btb_update(pc, target)
                    if ptarget != target:
                        if ptarget is None:          # no prediction: front-end waits
                            if c + 1 > t_disp:
                                t_disp = c + 1
                                slot = 0
                        else:
                            if c > max_c:
                                max_c = c
                            if c > last_r:
                                last_r = c
                            rob[ri] = last_r
                            ri += 1
                            if ri == ROB:
                                ri = 0
                            if trace:
                                self._tr(False, d, i, c, mode, pc)
                            stats["mispredicts"] += 1
                            self._wrong_path(ptarget, mode, t_disp, slot, c, rob[ri:] + rob[:ri])
                            if c + penalty > t_disp:
                                t_disp = c + penalty
                                slot = 0
                            pc = npc
                            continue

                elif op == FENCE:
                    i = d if d > max_c else max_c
                    c = i + 1
                    if pend:
                        self._drain_all()
                    if c > t_disp:
                        t_disp = c
                        slot = 0

                elif op == RDCYCLE:
                    i = d if d > max_c else max_c
                    c = i + 1
                    if rd:
                        regs[rd] = i
                        ready[rd] = c
                        taint[rd] = 0
                    if pend:
                        self._drain_all()
                    if c > t_disp:
                        t_disp = c
                        slot = 0

                elif op == CLFLUSH:
                    i = ready[rs1]
                    if d > i:
                        i = d
                    c = i + 2
                    addr = (regs[rs1] + imm) & M64
                    if addr < KERNEL_BASE or addr >= USER_END:
                        self._fault(f"clflush of unmapped address {addr:#x}", pc, mode)
                    if addr < USER_BASE and not mode:
                        self._fault(f"user-mode clflush of kernel line {addr:#x}", pc, mode)
                    if pend:
                        self._drain(i)
                    caches.flush(addr)

                elif op == SYSCALL:
                    if mode:
                        self._fault("syscall in kernel mode", pc, mode)
                    i = d if d > max_c else max_c
                    c = i + sys_lat
                    if pend:
                        self._drain_all()
                    self.epc = pc + 4
                    mode = KERNEL
                    npc = self.kernel_entry
                    t_disp = c
                    slot = 0
                    stats["syscalls"] += 1

                elif op == SYSRET:
                    if not mode:
                        self._fault("sysret in user mode", pc, mode)
                    i = d if d > max_c else max_c
                    c = i + sys_lat
                    if pend:
                        self._drain_all()
                    mode = USER
                    npc = self.epc
                    t_disp = c
                    slot = 0

                elif op == HALT:
                    if mode:
                        self._fault("halt in kernel mode", pc, mode)
                    i = d if d > max_c else max_c
                    c = i + 1
                    if pend:
                        self._drain_all()
                    t_disp = c
                    slot = 0
                    if c > max_c:
                        max_c = c
                    last_r = max_c
                    rob[ri] = last_r
                    ri = (ri + 1) % ROB
                    if trace:
                        self._tr(False, d, i, c, mode, pc)
                    break

                else:   # NOP
                    i, c = d, d + 1

                # ---------------- complete / retire bookkeeping
                if c > max_c:
                    max_c = c
                if c > last_r:
                    last_r = c
                rob[ri] = last_r
                ri += 1
                if ri == ROB:
                    ri = 0
                if trace:
                    self._tr(False, d, i, c, mode, pc)
                pc = npc
        finally:
            self.ri = ri
            self.slot = slot
            self.t_disp, self.max_c, self.last_r, self.brmax = t_disp, max_c, last_r, brmax
            self.mode = mode
            stats["instrs"] += n
        return n

    # ------------------------------------------------------------------ wrong path
    def _wrong_path(self, pc, mode, t, slot, deadline, older):
        """Transiently execute from `pc` until the mispredicted branch resolves at
        cycle `deadline`.  `older` holds the retire cycles of the ROB_SIZE most
        recent correct-path instructions (oldest first; the last one is the
        branch).  All architectural effects are discarded; cache fills stay."""
        cfg = self.cfg
        regs = self.regs[:]                  # checkpoint = work on copies
        ready = self.ready[:]
        policy = self.policy
        track = bool(policy is not None and policy.track_taint)
        taint = self.taint[:] if track else None
        caches, pred, mem = self.caches, self.pred, self.mem
        ucode, kcode = self.ucode, self.kcode
        penalty = cfg.mispredict_penalty
        W = cfg.width
        meltdown = cfg.meltdown
        trace = self.trace
        sb = {}                              # private store buffer: addr -> byte
        ROB = len(older)
        j = 0
        nloads = 0
        while j < ROB - 1:                   # ROB full once the branch itself is reached
            d = t
            x = older[j]
            if x > d:
                d = t = x
                slot = 0
            if d >= deadline:
                break                        # squashed before this one could dispatch
            if pc >= KTEXT_BASE:
                k = (pc - KTEXT_BASE) >> 2
                if not mode or k >= len(kcode) or pc & 3:
                    break
                inst = kcode[k]
            else:
                k = (pc - UTEXT_BASE) >> 2
                if k < 0 or k >= len(ucode) or pc & 3:
                    break
                inst = ucode[k]
            j += 1
            slot += 1
            if slot >= W:
                t = d + 1
                slot = 0
            op, rd, rs1, rs2, imm, fn, lat = inst
            npc = pc + 4
            i = c = None

            if op <= ALU_LAST:
                i = ready[rs1]
                if d > i:
                    i = d
                if op <= RR_LAST:
                    if ready[rs2] > i:
                        i = ready[rs2]
                    v = fn(regs[rs1], regs[rs2])
                else:
                    v = fn(regs[rs1], imm)
                c = i + lat
                if rd:
                    regs[rd] = v
                    ready[rd] = c
                    if track:
                        t1 = taint[rs1]
                        if op <= RR_LAST and taint[rs2] > t1:
                            t1 = taint[rs2]
                        taint[rd] = t1

            elif op <= LOAD_LAST:
                i = ready[rs1]
                if d > i:
                    i = d
                addr = (regs[rs1] + imm) & M64
                ev = None
                if policy is not None:
                    ev = LoadEvent(addr, i, True, INF, mode == KERNEL)
                    if track:
                        t2 = policy.transmit_delay(ev, taint[rs1])
                        if t2 > i:
                            i = t2
                            ev.t = i
                mapped = KERNEL_BASE <= addr and addr + fn <= USER_END
                allowed = mapped and (mode or addr >= USER_BASE)
                if i >= deadline or not (allowed or (meltdown and mapped)):
                    # never issued before the squash, or a suppressed fault: no data
                    if rd:
                        ready[rd] = INF
                    i = None
                else:
                    if sb:
                        v = int.from_bytes(bytes(sb.get(addr + q, mem[addr + q])
                                                 for q in range(fn)), "little")
                    else:
                        v = mem[addr] if fn == 1 else int.from_bytes(mem[addr:addr + fn], "little")
                    if self.pending:
                        self._drain(i)
                    if ev is None:
                        c = i + caches.access(addr, i)   # the transient footprint
                    else:
                        caches.now = i
                        c = i + policy.on_load(ev, caches)
                    nloads += 1
                    if rd:
                        regs[rd] = v
                        ready[rd] = c
                        if track:
                            taint[rd] = policy.load_taint(ev)

            elif op <= STORE_LAST:
                i = ready[rs1]
                if d > i:
                    i = d
                if ready[rs2] > i:
                    i = ready[rs2]
                addr = (regs[rs1] + imm) & M64
                if i < deadline and KERNEL_BASE <= addr and addr + fn <= USER_END \
                        and (mode or addr >= USER_BASE):
                    v = regs[rs2]
                    for q in range(fn):
                        sb[addr + q] = (v >> (8 * q)) & 0xFF
                    c = i + 1
                else:
                    i = None

            elif op <= BRANCH_LAST:
                ptaken = pred.predict(pc)
                i = ready[rs1]
                if d > i:
                    i = d
                if ready[rs2] > i:
                    i = ready[rs2]
                c = i + 1
                if c < deadline:
                    taken = fn(regs[rs1], regs[rs2])
                    if taken != ptaken:      # nested mispredict resolves first: redirect
                        ptaken = taken
                        if c + penalty > t:
                            t = c + penalty
                            slot = 0
                npc = imm if ptaken else pc + 4

            elif op == JAL:
                if rd:
                    regs[rd] = pc + 4
                    ready[rd] = d + 1
                    if track:
                        taint[rd] = 0
                npc = imm

            elif op == JALR:
                i = ready[rs1]
                if d > i:
                    i = d
                c = i + 1
                target = (regs[rs1] + imm) & M64
                ptarget = pred.btb_predict(pc)
                if rd:
                    regs[rd] = pc + 4
                    ready[rd] = d + 1
                    if track:
                        taint[rd] = 0
                if c < deadline:
                    if ptarget != target:
                        x = c + (penalty if ptarget is not None else 1)
                        if x > t:
                            t = x
                            slot = 0
                    npc = target
                elif ptarget is None:
                    break
                else:
                    npc = ptarget

            elif op == CLFLUSH or op == NOP:
                pass                         # clflush has no effect while speculative

            else:
                break                        # fence/rdcycle/syscall/sysret/halt serialize

            if trace:
                self._tr(True, d, i, c, mode, pc)
            pc = npc
        self.stats["transient"] += j
        self.stats["transient_loads"] += nloads
