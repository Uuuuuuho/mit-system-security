"""Helpers for writing assembly-to-assembly compiler passes (Part 4).

    from specsim import asmtools
    lines = asmtools.parse(src)       # list[Line]
    for ln in lines:
        ln.label        # 'loop' for a label line, else None
        ln.op           # 'bgeu', 'ld', '.data', ... (None for label lines)
        ln.args         # ['a0', 't1', 'out']
        ln.is_cond_branch / ln.is_load / ln.is_store / ln.is_directive
        ln.target       # branch target label (conditional branches only)
    out_src = asmtools.render(list_of_Lines_or_strings)

parse() puts every label on its own Line and canonicalizes the conditional
branch pseudo-instructions (beqz, bnez, bgt, ble, bgtu, bleu) into the six real
ones (beq, bne, blt, bge, bltu, bgeu) so a pass only has to handle those.
Comments are dropped.
"""

import itertools

from .asm import split_statement, expand_pseudo
from .isa import COND_BRANCH_MNEMONICS, LOAD_MNEMONICS, STORE_MNEMONICS

_BRANCH_PSEUDO = {"beqz", "bnez", "bgt", "ble", "bgtu", "bleu"}
_counter = itertools.count()


class Line:
    __slots__ = ("label", "op", "args", "lineno")

    def __init__(self, label=None, op=None, args=None, lineno=0):
        self.label, self.op, self.args, self.lineno = label, op, list(args or []), lineno

    @property
    def is_label(self):
        return self.label is not None

    @property
    def is_directive(self):
        return self.op is not None and self.op.startswith(".")

    @property
    def is_cond_branch(self):
        return self.op in COND_BRANCH_MNEMONICS

    @property
    def is_load(self):
        return self.op in LOAD_MNEMONICS

    @property
    def is_store(self):
        return self.op in STORE_MNEMONICS

    @property
    def target(self):
        if not self.is_cond_branch:
            raise ValueError(f"not a conditional branch: {self}")
        return self.args[2]

    def __str__(self):
        if self.label is not None:
            return f"{self.label}:"
        if self.is_directive and self.op in (".text", ".data", ".entry"):
            return f"{self.op}"
        return f"    {self.op} {', '.join(self.args)}".rstrip()

    __repr__ = __str__


def parse(src):
    out = []
    for lineno, text in enumerate(src.splitlines(), 1):
        labels, mnem, ops = split_statement(text)
        for lab in labels:
            out.append(Line(label=lab, lineno=lineno))
        if mnem is None:
            continue
        if mnem in _BRANCH_PSEUDO:
            mnem, ops = expand_pseudo(mnem, ops)
        out.append(Line(op=mnem, args=ops, lineno=lineno))
    return out


def render(lines):
    """Join Line objects and/or raw strings into assembly source text."""
    return "\n".join(str(x) for x in lines) + "\n"


def new_label(prefix="L"):
    """A fresh label name that cannot clash with labels in the sources here."""
    return f"__{prefix}_{next(_counter)}"


def branch_taken(op, rs1, rs2, rd):
    """Assembly lines that set register `rd` to 1 if the conditional branch
    `op rs1, rs2, ...` is taken and to 0 otherwise (branch-free, using only rd;
    rd may equal rs1 or rs2)."""
    if op == "beq":
        return [f"    xor {rd}, {rs1}, {rs2}", f"    sltiu {rd}, {rd}, 1"]
    if op == "bne":
        return [f"    xor {rd}, {rs1}, {rs2}", f"    sltu {rd}, zero, {rd}"]
    if op == "blt":
        return [f"    slt {rd}, {rs1}, {rs2}"]
    if op == "bge":
        return [f"    slt {rd}, {rs1}, {rs2}", f"    xori {rd}, {rd}, 1"]
    if op == "bltu":
        return [f"    sltu {rd}, {rs1}, {rs2}"]
    if op == "bgeu":
        return [f"    sltu {rd}, {rs1}, {rs2}", f"    xori {rd}, {rd}, 1"]
    raise ValueError(f"not a conditional branch: {op}")
