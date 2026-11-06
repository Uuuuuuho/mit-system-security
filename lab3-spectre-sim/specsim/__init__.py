"""specsim: a small transient-execution CPU model for teaching Spectre.

Public API (see README.md):
    Machine, Config          the simulated computer and its parameters
    Policy, LoadEvent        hooks for hardware defenses (Part 4)
    asmtools                 helpers for assembly-to-assembly passes (Part 4)
    ProgramFault, BudgetExceeded, RunLimit, AsmError
"""

from .asm import AsmError, assemble
from .cpu import ProgramFault, RunLimit
from .machine import Machine, Config, RunResult, BudgetExceeded, kernel_source
from .policy import Policy, LoadEvent, INF
from . import asmtools, layout

__all__ = ["Machine", "Config", "RunResult", "Policy", "LoadEvent", "INF", "asmtools", "layout",
           "ProgramFault", "RunLimit", "BudgetExceeded", "AsmError", "assemble", "kernel_source"]
