"""Machine: the public handle your attack code gets.

    m = Machine(seed=1)
    prog = m.load(asm_source)               # assemble + load a user program
    res = m.run(prog, a0=5)                 # run from _start with a0 = 5
    times = m.read_u64s(prog["times"], 256) # read your own (user) memory

Rules of the game: attack code may only use the public API below (load, run,
read*, write*, symbols, ksyms, constants, geometry, user).  Attributes starting
with an underscore (m._cpu, m._secret, ...) are simulator internals used by the
grader; touching them from attack code is cheating.
"""

import dataclasses
import os
import random
import string

from . import layout
from .asm import assemble
from .cache import CacheHierarchy
from .cpu import CPU, USER, ProgramFault, RunLimit
from .isa import REGS, REG_NAMES
from .predictor import BranchPredictor

_KERNEL_SRC_PATH = os.path.join(os.path.dirname(__file__), "kernel.s")


@dataclasses.dataclass
class Config:
    seed: int = 0
    width: int = 4                 # instructions dispatched per cycle
    rob: int = 128                 # ROB entries (bounds the speculation window)
    l1_sets: int = 64
    l1_ways: int = 8
    l2_sets: int = 2048
    l2_ways: int = 16
    l2_repl: str = "lru"           # L2 replacement: "lru" or "random" (Going further)
    lat_l1: int = 4
    lat_l2: int = 14
    lat_mem: int = 150
    spike_prob: float = 0.002      # probability of a +80..300 cycle "interrupt" per access
    mispredict_penalty: int = 10
    syscall_latency: int = 30
    pht_bits: int = 10
    ghist_bits: int = 0            # 0 = per-PC bimodal PHT; >0 = gshare
    meltdown: bool = False         # forward faulting data on wrong paths (Going further)
    policy: object = None          # Policy instance (Part 4 hardware defenses)
    kernel_pass: object = None     # callable(str) -> str applied to kernel.s (Part 4 software defenses)


class BudgetExceeded(Exception):
    """The grader's instruction budget for this task ran out."""


class RunResult:
    def __init__(self, cycles, instrs, regs):
        self.cycles = cycles
        self.instrs = instrs
        self.regs = regs

    def __getitem__(self, name):
        return self.regs[name]

    def __repr__(self):
        return f"RunResult(cycles={self.cycles}, instrs={self.instrs}, a0={self.regs['a0']:#x})"


def make_flag(rng, min_len=32, max_len=48):
    alphabet = string.ascii_lowercase + string.digits + "_"
    n = rng.randint(min_len, max_len)
    return ("FLAG{" + "".join(rng.choice(alphabet) for _ in range(n - 6)) + "}").encode()


def kernel_source():
    with open(_KERNEL_SRC_PATH) as f:
        return f.read()


class Machine:
    # layout constants, also available as assembler symbols
    PROBE_BASE = layout.PROBE_BASE
    PROBE_STRIDE = layout.PROBE_STRIDE
    PROBE_ENTRIES = layout.PROBE_ENTRIES
    HEAP_BASE = layout.HEAP_BASE
    HEAP_END = layout.HEAP_END
    USER_BASE = layout.USER_BASE
    USER_END = layout.USER_END
    KERNEL_BASE = layout.KERNEL_BASE
    KERNEL_END = layout.KERNEL_END
    STACK_TOP = layout.STACK_TOP
    LINE_SIZE = 64
    SYS_NOP = layout.SYS_NOP
    SYS_FR = layout.SYS_FR
    SYS_V1 = layout.SYS_V1
    SYS_V1_HARD = layout.SYS_V1_HARD

    def __init__(self, cfg=None, seed=None, **overrides):
        cfg = dataclasses.replace(cfg) if cfg is not None else Config()
        if seed is not None:
            cfg.seed = seed
        for k, v in overrides.items():
            if not hasattr(cfg, k):
                raise TypeError(f"unknown Config field {k!r}")
            setattr(cfg, k, v)
        self.cfg = cfg
        self.geometry = {"line": 64, "l1_sets": cfg.l1_sets, "l1_ways": cfg.l1_ways,
                         "l2_sets": cfg.l2_sets, "l2_ways": cfg.l2_ways,
                         "l2_inclusive": True, "l2_replacement": cfg.l2_repl,
                         "lat_l1": cfg.lat_l1, "lat_l2": cfg.lat_l2, "lat_mem": cfg.lat_mem}
        self._mem = bytearray(layout.MEM_END)
        caches = CacheHierarchy(cfg, seed=cfg.seed * 1_000_003 + 17)
        pred = BranchPredictor(cfg.pht_bits, cfg.ghist_bits)
        self._cpu = CPU(cfg, self._mem, caches, pred, cfg.policy)
        self._programs = {}
        self._data_next = layout.USER_DATA_BASE
        self.user = {}            # scratch dict for your own caching between calls
        self.budget = None        # remaining instruction budget (set by the grader)
        self.instructions = 0     # correct-path instructions executed so far
        self._boot(random.Random(cfg.seed * 7_919 + 3))

    # ------------------------------------------------------------------ boot
    def _boot(self, rng):
        src = kernel_source()
        if self.cfg.kernel_pass is not None:
            src = self.cfg.kernel_pass(src)
            if not isinstance(src, str):
                raise TypeError("kernel_pass must return the transformed source as a str")
        kprog = assemble(src, layout.KTEXT_BASE, layout.KERNEL_BASE,
                         layout.PUBLIC_CONSTANTS, name="kernel.s")
        if kprog.data_end > layout.KERNEL_END:
            raise MemoryError("kernel data too large")
        self._kprog = kprog
        self._mem[layout.KERNEL_BASE:kprog.data_end] = kprog.data
        cpu = self._cpu
        cpu.kcode = kprog.code
        cpu.ksrc = [("kernel.s", ln, txt) for ln, txt in kprog.src]
        cpu.kernel_entry = kprog.labels["syscall_entry"]
        self.ksyms = {k: v for k, v in kprog.labels.items() if k.startswith("k_")}
        self.symbols = dict(layout.PUBLIC_CONSTANTS)
        self.symbols.update(self.ksyms)
        # secrets and public tables
        self._secrets = {1: make_flag(rng), 2: make_flag(rng), 3: make_flag(rng)}
        for part, sym in ((1, "k_secret1"), (2, "k_secret2"), (3, "k_secret3")):
            s = self._secrets[part]
            self._kwrite(self.ksyms[sym], s + bytes(64 - len(s)))
        self._kwrite(self.ksyms["k_fr_len"], len(self._secrets[1]).to_bytes(8, "little"))
        self._kwrite(self.ksyms["k_v1_array"], bytes(rng.randrange(256) for _ in range(16)))
        self._kwrite(self.ksyms["k_v1h_array"], bytes(rng.randrange(256) for _ in range(16)))

    def _kwrite(self, addr, data):
        self._mem[addr:addr + len(data)] = data

    def _secret(self, part):
        """Grader only."""
        return self._secrets[part]

    # ------------------------------------------------------------------ programs
    def load(self, source, name="user.s"):
        """Assemble `source` and load it.  Loading the exact same source again
        returns the already-loaded Program (its memory is NOT re-initialized), so
        it is cheap to call load() every time.  Pass per-run values in registers
        (m.run(prog, a0=...)) instead of generating a new source each time: each
        distinct source permanently takes space in the 2 MiB program data area."""
        prog = self._programs.get(source)
        if prog is not None:
            return prog
        cpu = self._cpu
        data_base = (self._data_next + 63) & ~63
        text_base = layout.UTEXT_BASE + 4 * len(cpu.ucode)
        prog = assemble(source, text_base, data_base, self.symbols, name=name)
        if prog.data_end > layout.USER_DATA_END:
            raise MemoryError("program data area (2 MiB) exhausted: put big buffers in the "
                              "HEAP (HEAP_BASE..HEAP_END) and reuse programs")
        self._mem[data_base:prog.data_end] = prog.data
        cpu.ucode.extend(prog.code)
        cpu.usrc.extend((name, ln, txt) for ln, txt in prog.src)
        self._data_next = prog.data_end
        self._programs[source] = prog
        return prog

    def run(self, prog, entry="_start", max_instrs=5_000_000, trace=None, **regs):
        """Run a loaded program (or a source string) in user mode from `entry`
        until it executes `halt`.  Keyword arguments set registers (a0=..., s1=...);
        all other registers start at 0, sp at STACK_TOP.  Caches, predictor and
        memory persist across runs.  `trace=print` prints a per-instruction
        timing trace ('~' marks transient, squashed instructions)."""
        if isinstance(prog, str):
            prog = self.load(prog)
        cpu = self._cpu
        limit = max_instrs
        if self.budget is not None:
            if self.budget <= 0:
                raise BudgetExceeded("instruction budget exhausted")
            limit = min(limit, self.budget)
        vals = [0] * 32
        vals[REGS["sp"]] = layout.STACK_TOP
        for name, v in regs.items():
            r = REGS.get(name.lower())
            if r is None or r == 0:
                raise ValueError(f"bad register {name!r}")
            vals[r] = v & ((1 << 64) - 1)
        cpu.regs[:] = vals
        cpu.mode = USER
        cpu.trace = (print if trace is True else trace) or None
        start_cycle = cpu.t_disp
        before = cpu.stats["instrs"]
        try:
            n = cpu.run(prog.addr(entry), limit)
        except RunLimit:
            if self.budget is not None and limit == self.budget:
                self.budget = 0
                raise BudgetExceeded("instruction budget exhausted") from None
            raise
        finally:
            cpu.trace = None
            used = cpu.stats["instrs"] - before
            self.instructions += used
            if self.budget is not None:
                self.budget = max(0, self.budget - used)
        return RunResult(cpu.t_disp - start_cycle, n,
                         {REG_NAMES[i]: cpu.regs[i] for i in range(32)})

    # ------------------------------------------------------------------ user memory
    def _check_user(self, addr, n):
        if not (layout.USER_BASE <= addr and addr + n <= layout.USER_END):
            raise PermissionError(f"{addr:#x}+{n} is not user memory")

    def read(self, addr, n):
        self._check_user(addr, n)
        return bytes(self._mem[addr:addr + n])

    def read_u64(self, addr):
        return int.from_bytes(self.read(addr, 8), "little")

    def read_u64s(self, addr, count):
        b = self.read(addr, 8 * count)
        return [int.from_bytes(b[8 * k:8 * k + 8], "little") for k in range(count)]

    def write(self, addr, data):
        self._check_user(addr, len(data))
        self._mem[addr:addr + len(data)] = data

    def write_u64s(self, addr, values):
        self.write(addr, b"".join((v & ((1 << 64) - 1)).to_bytes(8, "little") for v in values))

    def now(self):
        """Current cycle count of the machine."""
        return self._cpu.t_disp


__all__ = ["Machine", "Config", "RunResult", "BudgetExceeded", "ProgramFault", "RunLimit"]
