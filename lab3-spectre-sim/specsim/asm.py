"""Two-pass assembler for the specsim toy ISA.

Syntax summary (full reference in README.md):

    # comment            ; also a comment
    .equ NAME, expr      define a constant (alias: .set)
    .text / .data        switch section (default .text)
    label:               labels may share a line with an instruction
    .entry               mark an entry point (used by the Part 4 compiler passes)
    .byte/.word/.quad e, e, ...    1/4/8-byte little-endian data
    .zero N  (.space N)  N zero bytes
    .align N (.balign N) align to N bytes (N a power of two)
    .ascii "s" / .asciz "s"

Operands are registers (RISC-V ABI names or x0..x31), expressions, or memory
operands `expr(reg)`.  Expressions may use + - * / % << >> & | ^ ~, parentheses,
character literals 'A', and any symbol: labels, .equ constants and the
predefined symbols the Machine provides (PROBE_BASE, SYS_V1, kernel symbols...).
"""

import ast
import re

from . import isa

_LABEL_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:(?!:)")
_MEM_RE = re.compile(r"^(.*)\(\s*([A-Za-z0-9]+)\s*\)\s*$")
_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class AsmError(Exception):
    pass


class Program:
    """An assembled program: decoded code, initial data and a symbol table."""

    def __init__(self, name, text_base, data_base):
        self.name = name
        self.text_base = text_base
        self.data_base = data_base
        self.code = []          # decoded instruction tuples
        self.src = []           # (line number, source text) per instruction
        self.data = bytearray()
        self.symbols = {}       # every symbol: labels, .equ, predefined
        self.labels = {}        # labels defined by this program only
        self.entries = []       # PCs marked with .entry

    def addr(self, name):
        """Address of a label or value of a symbol."""
        try:
            return self.symbols[name]
        except KeyError:
            raise KeyError(f"{self.name}: unknown symbol {name!r}") from None

    __getitem__ = addr

    @property
    def text_end(self):
        return self.text_base + 4 * len(self.code)

    @property
    def data_end(self):
        return self.data_base + len(self.data)


# ---------------------------------------------------------------------------
# lexing helpers
# ---------------------------------------------------------------------------
def strip_comment(line):
    """Remove a trailing '#', ';' or '//' comment (quotes are respected)."""
    quote = None
    i = 0
    while i < len(line):
        ch = line[i]
        if quote:
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = None
        elif ch in "\"'":
            quote = ch
        elif ch in "#;" or line.startswith("//", i):
            return line[:i]
        i += 1
    return line


def split_operands(s):
    """Split 'a, b(c), "x,y"' on top-level commas."""
    out, cur, depth, quote = [], [], 0, None
    i = 0
    while i < len(s):
        ch = s[i]
        if quote:
            cur.append(ch)
            if ch == "\\" and i + 1 < len(s):
                cur.append(s[i + 1])
                i += 2
                continue
            if ch == quote:
                quote = None
        elif ch in "\"'":
            quote = ch
            cur.append(ch)
        elif ch == "(":
            depth += 1
            cur.append(ch)
        elif ch == ")":
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
        i += 1
    tail = "".join(cur).strip()
    if tail or out:
        out.append(tail)
    return out


def split_statement(line):
    """Return (labels, mnemonic or None, operand list) for one source line."""
    text = strip_comment(line)
    labels = []
    while True:
        m = _LABEL_RE.match(text)
        if not m:
            break
        labels.append(m.group(1))
        text = text[m.end():]
    text = text.strip()
    if not text:
        return labels, None, []
    parts = text.split(None, 1)
    mnem = parts[0].lower()
    ops = split_operands(parts[1]) if len(parts) > 1 else []
    return labels, mnem, ops


_BINOPS = {
    ast.Add: lambda a, b: a + b, ast.Sub: lambda a, b: a - b,
    ast.Mult: lambda a, b: a * b, ast.FloorDiv: lambda a, b: a // b,
    ast.Div: lambda a, b: a // b, ast.Mod: lambda a, b: a % b,
    ast.LShift: lambda a, b: a << b, ast.RShift: lambda a, b: a >> b,
    ast.BitAnd: lambda a, b: a & b, ast.BitOr: lambda a, b: a | b,
    ast.BitXor: lambda a, b: a ^ b,
}
_UNOPS = {ast.USub: lambda a: -a, ast.UAdd: lambda a: a, ast.Invert: lambda a: ~a}


def eval_expr(expr, symbols):
    """Evaluate an integer expression; raises AsmError on problems."""
    expr = expr.strip()
    if not expr:
        raise AsmError("missing expression")
    if _IDENT_RE.match(expr):            # fast path; also allows labels like `pass`
        if expr in symbols:
            return symbols[expr]
        if expr.lower() in isa.REGS:
            raise AsmError(f"register {expr!r} used where a value is expected")
        raise AsmError(f"undefined symbol {expr!r}")
    try:
        node = ast.parse(expr, mode="eval").body
    except SyntaxError:
        raise AsmError(f"bad expression {expr!r}") from None

    def ev(n):
        if isinstance(n, ast.Constant):
            if isinstance(n.value, bool):
                raise AsmError(f"bad constant in {expr!r}")
            if isinstance(n.value, int):
                return n.value
            if isinstance(n.value, str) and len(n.value) == 1:
                return ord(n.value)
            raise AsmError(f"bad constant {n.value!r} in {expr!r}")
        if isinstance(n, ast.Name):
            if n.id in symbols:
                return symbols[n.id]
            if n.id.lower() in isa.REGS:
                raise AsmError(f"register {n.id!r} used where a value is expected")
            raise AsmError(f"undefined symbol {n.id!r}")
        if isinstance(n, ast.BinOp) and type(n.op) in _BINOPS:
            return _BINOPS[type(n.op)](ev(n.left), ev(n.right))
        if isinstance(n, ast.UnaryOp) and type(n.op) in _UNOPS:
            return _UNOPS[type(n.op)](ev(n.operand))
        raise AsmError(f"unsupported expression {expr!r}")

    return ev(node)


def parse_reg(tok):
    r = isa.REGS.get(tok.strip().lower())
    if r is None:
        raise AsmError(f"bad register {tok!r}")
    return r


def parse_mem(tok, symbols):
    m = _MEM_RE.match(tok)
    if not m:
        raise AsmError(f"bad memory operand {tok!r} (expected off(reg))")
    off = m.group(1).strip()
    return (eval_expr(off, symbols) if off else 0), parse_reg(m.group(2))


def expand_pseudo(mnem, ops):
    """Rewrite a pseudo-instruction into a real mnemonic + operands."""
    if mnem == "mv":
        return "addi", [ops[0], ops[1], "0"]
    if mnem == "not":
        return "xori", [ops[0], ops[1], "-1"]
    if mnem == "neg":
        return "sub", [ops[0], "zero", ops[1]]
    if mnem in ("beqz", "bnez"):
        return mnem[:3], [ops[0], "zero", ops[1]]
    if mnem in ("bgt", "ble", "bgtu", "bleu"):
        real = {"bgt": "blt", "ble": "bge", "bgtu": "bltu", "bleu": "bgeu"}[mnem]
        return real, [ops[1], ops[0], ops[2]]
    if mnem == "j":
        return "jal", ["zero", ops[0]]
    if mnem == "call":
        return "jal", ["ra", ops[0]]
    if mnem == "jr":
        return "jalr", ["zero", ops[0], "0"]
    if mnem == "ret":
        return "jalr", ["zero", "ra", "0"]
    return mnem, ops


_DATA_DIRECTIVES = {".byte": 1, ".word": 4, ".quad": 8, ".dword": 8}


def _string_bytes(tok):
    try:
        s = ast.literal_eval(tok)
    except Exception:
        raise AsmError(f"bad string literal {tok}") from None
    if not isinstance(s, str):
        raise AsmError(f"bad string literal {tok}")
    return s.encode("latin-1")


# ---------------------------------------------------------------------------
# the assembler
# ---------------------------------------------------------------------------
def assemble(source, text_base, data_base, predefined=None, name="<asm>"):
    """Assemble `source`; text is placed at `text_base`, data at `data_base`."""
    prog = Program(name, text_base, data_base)
    symbols = dict(predefined or {})
    lines = source.splitlines()

    # ---- pass 1: labels, sizes, constants --------------------------------
    section = "text"
    tpc, dloc = text_base, data_base
    stmts = []   # (lineno, text, section, mnemonic, operands, data_loc)

    def err(lineno, text, msg):
        return AsmError(f"{name}:{lineno}: {msg}\n    {text.strip()}")

    for lineno, text in enumerate(lines, 1):
        try:
            labels, mnem, ops = split_statement(text)
        except AsmError as e:
            raise err(lineno, text, e) from None
        for lab in labels:
            if lab in prog.labels:
                raise err(lineno, text, f"label {lab!r} defined twice")
            if lab.lower() in isa.REGS:
                raise err(lineno, text, f"label {lab!r} clashes with a register name")
            value = tpc if section == "text" else dloc
            prog.labels[lab] = value
            symbols[lab] = value
        if mnem is None:
            continue
        try:
            if mnem in (".text", ".data"):
                section = mnem[1:]
                continue
            if mnem in (".globl", ".global", ".type", ".size", ".section"):
                continue
            if mnem in (".equ", ".set"):
                if len(ops) != 2 or not _IDENT_RE.match(ops[0]):
                    raise AsmError(".equ needs NAME, expr")
                symbols[ops[0]] = eval_expr(ops[1], symbols)
                continue
            if mnem == ".entry":
                if section != "text":
                    raise AsmError(".entry must be in .text")
                prog.entries.append(tpc)
                continue
            if mnem.startswith("."):
                if section != "data":
                    raise AsmError(f"data directive {mnem} outside .data")
                start = dloc
                if mnem in _DATA_DIRECTIVES:
                    dloc += _DATA_DIRECTIVES[mnem] * len(ops)
                elif mnem in (".zero", ".space"):
                    n = eval_expr(ops[0], symbols)
                    if n < 0:
                        raise AsmError("negative size")
                    dloc += n
                elif mnem in (".align", ".balign"):
                    a = eval_expr(ops[0], symbols)
                    if a <= 0 or a & (a - 1):
                        raise AsmError(".align needs a power of two (in bytes)")
                    dloc = (dloc + a - 1) & ~(a - 1)
                elif mnem in (".ascii", ".asciz"):
                    b = b"".join(_string_bytes(o) for o in ops)
                    dloc += len(b) + (1 if mnem == ".asciz" else 0)
                else:
                    raise AsmError(f"unknown directive {mnem}")
                stmts.append((lineno, text, "data", mnem, ops, start))
                continue
            if section != "text":
                raise AsmError(f"instruction {mnem!r} in .data section")
            if mnem not in isa.MNEMONICS and mnem not in isa.PSEUDO:
                raise AsmError(f"unknown instruction {mnem!r}")
            stmts.append((lineno, text, "text", mnem, ops, tpc))
            tpc += 4
        except AsmError as e:
            raise err(lineno, text, e) from None

    prog.symbols = symbols
    text_end = tpc

    # ---- pass 2: encode ---------------------------------------------------
    data = bytearray(dloc - data_base)
    for lineno, text, sect, mnem, ops, loc in stmts:
        try:
            if sect == "data":
                off = loc - data_base
                if mnem in _DATA_DIRECTIVES:
                    size = _DATA_DIRECTIVES[mnem]
                    mask = (1 << (8 * size)) - 1
                    for o in ops:
                        v = eval_expr(o, symbols) & mask
                        data[off:off + size] = v.to_bytes(size, "little")
                        off += size
                elif mnem in (".ascii", ".asciz"):
                    b = b"".join(_string_bytes(o) for o in ops)
                    data[off:off + len(b)] = b
                continue
            prog.code.append(_encode(mnem, ops, symbols, loc, text_base, text_end))
            prog.src.append((lineno, text.strip()))
        except AsmError as e:
            raise err(lineno, text, e) from None
        except IndexError:
            raise err(lineno, text, "missing operand") from None
    prog.data = data
    return prog


def _encode(mnem, ops, symbols, pc, text_base, text_end):
    mnem, ops = expand_pseudo(mnem, ops)
    op, fmt = isa.MNEMONICS[mnem]

    def nops(*counts):
        if len(ops) not in counts:
            raise AsmError(f"{mnem} expects {' or '.join(map(str, counts))} operands, got {len(ops)}")

    def code_target(expr):
        t = eval_expr(expr, symbols)
        if not (text_base <= t < text_end) or t & 3:
            raise AsmError(f"branch/jump target {expr!r} is not a code label of this program")
        return t

    if fmt == "R":
        nops(3)
        return (op, parse_reg(ops[0]), parse_reg(ops[1]), parse_reg(ops[2]), 0,
                isa.ALU_FN[op], isa.ALU_LAT[op])
    if fmt == "I":
        nops(3)
        imm = eval_expr(ops[2], symbols) & isa.M64
        return (op, parse_reg(ops[0]), parse_reg(ops[1]), 0, imm, isa.ALU_FN[op], isa.ALU_LAT[op])
    if fmt == "LI":
        nops(2)
        imm = eval_expr(ops[1], symbols) & isa.M64
        return (op, parse_reg(ops[0]), 0, 0, imm, isa.ALU_FN[op], 1)
    if fmt == "L":
        nops(2)
        off, base = parse_mem(ops[1], symbols)
        return (op, parse_reg(ops[0]), base, 0, off, isa.MEM_SIZE[op], 0)
    if fmt == "S":
        nops(2)
        off, base = parse_mem(ops[1], symbols)
        return (op, 0, base, parse_reg(ops[0]), off, isa.MEM_SIZE[op], 0)
    if fmt == "B":
        nops(3)
        return (op, 0, parse_reg(ops[0]), parse_reg(ops[1]), code_target(ops[2]),
                isa.BRANCH_FN[op], 1)
    if fmt == "JAL":
        nops(1, 2)
        rd = parse_reg(ops[0]) if len(ops) == 2 else isa.REGS["ra"]
        return (op, rd, 0, 0, code_target(ops[-1]), None, 1)
    if fmt == "JALR":
        nops(2, 3)
        rd = parse_reg(ops[0])
        if len(ops) == 2 and "(" in ops[1]:
            off, base = parse_mem(ops[1], symbols)
        else:
            base = parse_reg(ops[1])
            off = eval_expr(ops[2], symbols) if len(ops) == 3 else 0
        return (op, rd, base, 0, off, None, 1)
    if fmt == "C":
        nops(1)
        off, base = parse_mem(ops[0], symbols)
        return (op, 0, base, 0, off, None, 1)
    if fmt == "RD":
        nops(1)
        return (op, parse_reg(ops[0]), 0, 0, 0, None, 1)
    if fmt == "N":
        nops(0)
        return (op, 0, 0, 0, 0, None, 1)
    raise AsmError(f"internal: unknown format {fmt}")
