#!/usr/bin/env python3
"""Tiny assembler / disassembler for the psp-mini ISA (RV32I subset).

    rvasm.py prog.s -o prog.hex      assemble (one 32-bit hex word per line)
    rvasm.py -d 0x00500093 ...       disassemble words

Supported instructions
    lui rd, imm20            jal rd, label        beq rs1, rs2, label
    addi rd, rs1, imm12      srli rd, rs1, shamt  add rd, rs1, rs2
    xor rd, rs1, rs2         lw rd, off(rs1)      sw rs2, off(rs1)
    ecall                    mret                 csrrw rd, csr, rs1
Pseudo-instructions
    li rd, imm32   la rd, label (always lui+addi)   mv rd, rs   nop   j label
    csrw csr, rs   halt (= beq x0,x0,.)
    .word value
Registers: x0..x31 are accepted (psp-mini only decodes bits [1:0], so x5 is
x1!), plus the ABI names zero/ra/sp/gp for x0..x3.
CSRs: mstatus mtvec mepc mcause (or a number).
Comments start with '#'.  Lines starting with '#!' are test expectations and
are ignored here (tools/sim.py reads them).
"""
import re
import sys

OP_LUI, OP_JAL, OP_BRANCH, OP_LOAD, OP_STORE = 0x37, 0x6F, 0x63, 0x03, 0x23
OP_IMM, OP_REG, OP_SYSTEM, OP_CUSTOM0 = 0x13, 0x33, 0x73, 0x0B
CSRS = {"mstatus": 0x300, "mtvec": 0x305, "mepc": 0x341, "mcause": 0x342}
CSR_NAMES = {v: k for k, v in CSRS.items()}
ABI = {"zero": 0, "ra": 1, "sp": 2, "gp": 3}
NREGS = 4


class AsmError(Exception):
    pass


def reg(tok):
    tok = tok.strip().lower()
    if tok in ABI:
        return ABI[tok]
    m = re.fullmatch(r"x(\d+)", tok)
    if not m or int(m.group(1)) > 31:
        raise AsmError(f"bad register '{tok}'")
    return int(m.group(1))


def num(tok):
    tok = tok.strip().lower().replace("_", "")
    neg = tok.startswith("-")
    if neg:
        tok = tok[1:]
    v = int(tok, 0)
    return -v if neg else v


def check_range(v, lo, hi, what):
    if not lo <= v <= hi:
        raise AsmError(f"{what} {v} out of range [{lo}, {hi}]")


# ---------------------------------------------------------------- encoders --
def enc_r(f7, rs2, rs1, f3, rd, op):
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op


def enc_i(imm, rs1, f3, rd, op):
    check_range(imm, -2048, 2047, "12-bit immediate")
    return ((imm & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op


def enc_s(imm, rs2, rs1, f3, op):
    check_range(imm, -2048, 2047, "store offset")
    imm &= 0xFFF
    return ((imm >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((imm & 0x1F) << 7) | op


def enc_b(off, rs2, rs1, f3, op):
    check_range(off, -4096, 4094, "branch offset")
    if off & 1:
        raise AsmError("odd branch offset")
    o = off & 0x1FFF
    return (((o >> 12) & 1) << 31) | (((o >> 5) & 0x3F) << 25) | (rs2 << 20) | (rs1 << 15) | \
        (f3 << 12) | (((o >> 1) & 0xF) << 8) | (((o >> 11) & 1) << 7) | op


def enc_u(imm20, rd, op):
    check_range(imm20, 0, 0xFFFFF, "20-bit upper immediate")
    return (imm20 << 12) | (rd << 7) | op


def enc_j(off, rd, op):
    check_range(off, -(1 << 20), (1 << 20) - 2, "jump offset")
    o = off & 0x1FFFFF
    return (((o >> 20) & 1) << 31) | (((o >> 1) & 0x3FF) << 21) | (((o >> 11) & 1) << 20) | \
        (((o >> 12) & 0xFF) << 12) | (rd << 7) | op


def li_parts(v):
    """Split a 32-bit constant into (lui imm20, addi imm12)."""
    v &= 0xFFFFFFFF
    hi = ((v + 0x800) >> 12) & 0xFFFFF
    lo = v - (hi << 12)
    lo = ((lo + 0x800) & 0xFFF) - 0x800
    return hi, lo


def li_len(v):
    hi, lo = li_parts(v)
    return 1 if hi == 0 or lo == 0 else 2


def split_ops(rest):
    return [t.strip() for t in rest.split(",")] if rest.strip() else []


def size_of(mn, ops):
    if mn == "li":
        return 4 * li_len(num(ops[1]))
    if mn == "la":
        return 8
    return 4


def assemble(text, origin=0):
    """Return a list of 32-bit words."""
    lines = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        while True:
            m = re.match(r"^([A-Za-z_.][\w.]*):\s*(.*)$", line)
            if not m:
                break
            lines.append((lineno, "label", m.group(1), None))
            line = m.group(2)
        if line:
            parts = line.split(None, 1)
            lines.append((lineno, "insn", parts[0].lower(), split_ops(parts[1] if len(parts) > 1 else "")))
    # pass 1: labels
    labels, pc = {}, origin
    for lineno, kind, a, ops in lines:
        try:
            if kind == "label":
                if a in labels:
                    raise AsmError(f"duplicate label '{a}'")
                labels[a] = pc
            else:
                pc += size_of(a, ops)
        except (AsmError, ValueError, IndexError) as e:
            raise AsmError(f"line {lineno}: {e}")
    # pass 2: encode
    words, pc = [], origin

    def target(tok, absolute=False):
        tok = tok.strip()
        if tok in labels:
            return labels[tok] if absolute else labels[tok] - pc
        if tok == ".":
            return pc if absolute else 0
        return num(tok)

    for lineno, kind, mn, ops in lines:
        if kind == "label":
            continue
        try:
            out = encode(mn, ops, target)
        except (AsmError, ValueError, IndexError, KeyError) as e:
            raise AsmError(f"line {lineno}: {mn} {', '.join(ops)}: {e}")
        words.extend(out)
        pc += 4 * len(out)
    return words


def csr_num(tok):
    tok = tok.strip().lower()
    return CSRS[tok] if tok in CSRS else num(tok)


def mem_operand(tok):
    m = re.fullmatch(r"\s*(-?[\w]*)\s*\(\s*(\w+)\s*\)\s*", tok)
    if not m:
        raise AsmError(f"bad memory operand '{tok}' (want off(reg))")
    return (num(m.group(1)) if m.group(1) else 0), reg(m.group(2))


def encode(mn, ops, target):
    n = len(ops)

    def need(k):
        if n != k:
            raise AsmError(f"expected {k} operands")

    if mn == "lui":
        need(2)
        return [enc_u(num(ops[1]), reg(ops[0]), OP_LUI)]
    if mn == "addi":
        need(3)
        return [enc_i(num(ops[2]), reg(ops[1]), 0, reg(ops[0]), OP_IMM)]
    if mn == "srli":
        need(3)
        sh = num(ops[2])
        check_range(sh, 0, 31, "shift amount")
        return [enc_r(0, sh, reg(ops[1]), 5, reg(ops[0]), OP_IMM)]
    if mn == "add":
        need(3)
        return [enc_r(0, reg(ops[2]), reg(ops[1]), 0, reg(ops[0]), OP_REG)]
    if mn == "xor":
        need(3)
        return [enc_r(0, reg(ops[2]), reg(ops[1]), 4, reg(ops[0]), OP_REG)]
    if mn == "beq":
        need(3)
        return [enc_b(target(ops[2]), reg(ops[1]), reg(ops[0]), 0, OP_BRANCH)]
    if mn == "jal":
        if n == 1:
            return [enc_j(target(ops[0]), 1, OP_JAL)]
        need(2)
        return [enc_j(target(ops[1]), reg(ops[0]), OP_JAL)]
    if mn == "j":
        need(1)
        return [enc_j(target(ops[0]), 0, OP_JAL)]
    if mn == "lw":
        need(2)
        off, base = mem_operand(ops[1])
        return [enc_i(off, base, 2, reg(ops[0]), OP_LOAD)]
    if mn == "sw":
        need(2)
        off, base = mem_operand(ops[1])
        return [enc_s(off, reg(ops[0]), base, 2, OP_STORE)]
    if mn == "ecall":
        need(0)
        return [0x00000073]
    if mn == "mret":
        need(0)
        return [0x30200073]
    if mn == "csrrw":
        need(3)
        c = csr_num(ops[1])
        check_range(c, 0, 0xFFF, "CSR number")
        return [(c << 20) | (reg(ops[2]) << 15) | (1 << 12) | (reg(ops[0]) << 7) | OP_SYSTEM]
    if mn == "csrw":
        need(2)
        return encode("csrrw", ["x0", ops[0], ops[1]], target)
    if mn == "li":
        need(2)
        rd = reg(ops[0])
        hi, lo = li_parts(num(ops[1]))
        if hi == 0:
            return [enc_i(lo, 0, 0, rd, OP_IMM)]
        if lo == 0:
            return [enc_u(hi, rd, OP_LUI)]
        return [enc_u(hi, rd, OP_LUI), enc_i(lo, rd, 0, rd, OP_IMM)]
    if mn == "la":
        need(2)
        rd = reg(ops[0])
        hi, lo = li_parts(target(ops[1], absolute=True))
        return [enc_u(hi, rd, OP_LUI), enc_i(lo, rd, 0, rd, OP_IMM)]
    if mn == "mv":
        need(2)
        return [enc_i(0, reg(ops[1]), 0, reg(ops[0]), OP_IMM)]
    if mn == "nop":
        need(0)
        return [enc_i(0, 0, 0, 0, OP_IMM)]
    if mn == "halt":
        need(0)
        return [0x00000063]
    if mn == ".word":
        need(1)
        return [num(ops[0]) & 0xFFFFFFFF]
    raise AsmError(f"unknown mnemonic '{mn}'")


# ------------------------------------------------------------ disassembler --
def sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def rname(field):
    r = field & 3
    return f"x{r}" if field == r else f"x{field}(=x{r})"


def disasm(w, pc=None):
    """One-line disassembly of a 32-bit word (psp-mini semantics)."""
    w &= 0xFFFFFFFF
    op, rd, f3 = w & 0x7F, (w >> 7) & 0x1F, (w >> 12) & 7
    rs1, rs2, f7 = (w >> 15) & 0x1F, (w >> 20) & 0x1F, w >> 25
    imm_i = sext(w >> 20, 12)
    imm_s = sext(((w >> 25) << 5) | ((w >> 7) & 0x1F), 12)
    imm_b = sext((((w >> 31) & 1) << 12) | (((w >> 7) & 1) << 11) | (((w >> 25) & 0x3F) << 5) |
                 (((w >> 8) & 0xF) << 1), 13)
    imm_j = sext((((w >> 31) & 1) << 20) | (((w >> 12) & 0xFF) << 12) | (((w >> 20) & 1) << 11) |
                 (((w >> 21) & 0x3FF) << 1), 21)

    def tgt(off):
        return f"{off:+d}" if pc is None else f"0x{(pc + off) & 0xFFFFFFFF:x}"

    if op == OP_LUI:
        return f"lui {rname(rd)}, 0x{w >> 12:x}"
    if op == OP_JAL:
        return f"jal {rname(rd)}, {tgt(imm_j)}"
    if op == OP_BRANCH and f3 == 0:
        if w == 0x63:
            return "halt (beq x0, x0, .)"
        return f"beq {rname(rs1)}, {rname(rs2)}, {tgt(imm_b)}"
    if op == OP_LOAD and f3 == 2:
        return f"lw {rname(rd)}, {imm_i}({rname(rs1)})"
    if op == OP_STORE and f3 == 2:
        return f"sw {rname(rs2)}, {imm_s}({rname(rs1)})"
    if op == OP_IMM and f3 == 0:
        return f"addi {rname(rd)}, {rname(rs1)}, {imm_i}"
    if op == OP_IMM and f3 == 5 and f7 == 0:
        return f"srli {rname(rd)}, {rname(rs1)}, {rs2}"
    if op == OP_REG and f7 == 0 and f3 == 0:
        return f"add {rname(rd)}, {rname(rs1)}, {rname(rs2)}"
    if op == OP_REG and f7 == 0 and f3 == 4:
        return f"xor {rname(rd)}, {rname(rs1)}, {rname(rs2)}"
    if w == 0x00000073:
        return "ecall"
    if w == 0x30200073:
        return "mret"
    if op == OP_SYSTEM and f3 == 1:
        c = w >> 20
        cn = CSR_NAMES.get(c, f"0x{c:03x}")
        return f"csrrw {rname(rd)}, {cn}, {rname(rs1)}"
    if op == OP_CUSTOM0:
        return (f"custom-0 f3={f3} f7=0x{f7:02x} rd={rname(rd)} rs1={rname(rs1)} "
                f"rs2={rname(rs2)}  [reserved: must trap]")
    return "(illegal)"


def main(argv):
    if len(argv) >= 2 and argv[1] == "-d":
        for t in argv[2:]:
            w = int(t, 16) if not t.lower().startswith("0x") else int(t, 0)
            print(f"{w:08x}  {disasm(w)}")
        return 0
    if len(argv) < 2:
        print(__doc__)
        return 1
    src = argv[1]
    out = argv[argv.index("-o") + 1] if "-o" in argv else None
    try:
        words = assemble(open(src).read())
    except AsmError as e:
        print(f"{src}: error: {e}", file=sys.stderr)
        return 1
    text = "".join(f"{w:08x}\n" for w in words)
    if out:
        with open(out, "w") as f:
            f.write(text)
    else:
        for i, w in enumerate(words):
            print(f"{4 * i:04x}: {w:08x}  {disasm(w, 4 * i)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
