"""rv32.py - RV32I encoder/decoder and psp-lite test-harness helpers for Part 4 (PROVIDED).

Encoding:
    encode("addi", rd=5, rs1=0, imm=-3)       -> 32-bit instruction word
    encode("srai", rd=1, rs1=2, imm=7)        (shift amount goes in imm)
    encode("sw", rs1=31, rs2=4, imm=-8)
    encode("beq", rs1=1, rs2=2, imm=8)        (branch/jump offsets are byte offsets)
Decoding:
    decode(word) -> dict(name=..., rd=..., rs1=..., rs2=..., imm=...)   (name "illegal" if not RV32I)

Harness (a flat binary that psp-lite loads at 0x80000000):
    [prologue: x1..x30 <- init values (lui+addi), x31 <- DATA_BASE]
    [body words ...]
    [epilogue: csrw 0x802, x0   -> psp-lite exits with status 0]
    ... zero padding ...
    [DATA_SIZE bytes of data at offset DATA_OFF]
x31 points into the middle of the data block, so loads/stores "imm(x31)" with any
12-bit immediate stay inside it.  Keep x31 out of your generated rd fields.
"""
import os
import subprocess

BASE = 0x80000000
DATA_OFF = 0x4000
DATA_SIZE = 0x1000
BASE_REG = 31
DATA_BASE = BASE + DATA_OFF + DATA_SIZE // 2  # value of x31
MAX_BODY = (DATA_OFF // 4) - 64 - 2  # instruction slots available for the body

ABI = ["zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
       "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"]

# name: (format, opcode, funct3, funct7)
INSNS = {
    "lui": ("U", 0x37, None, None), "auipc": ("U", 0x17, None, None),
    "jal": ("J", 0x6F, None, None), "jalr": ("I", 0x67, 0, None),
    "beq": ("B", 0x63, 0, None), "bne": ("B", 0x63, 1, None), "blt": ("B", 0x63, 4, None),
    "bge": ("B", 0x63, 5, None), "bltu": ("B", 0x63, 6, None), "bgeu": ("B", 0x63, 7, None),
    "lb": ("I", 0x03, 0, None), "lh": ("I", 0x03, 1, None), "lw": ("I", 0x03, 2, None),
    "lbu": ("I", 0x03, 4, None), "lhu": ("I", 0x03, 5, None),
    "sb": ("S", 0x23, 0, None), "sh": ("S", 0x23, 1, None), "sw": ("S", 0x23, 2, None),
    "addi": ("I", 0x13, 0, None), "slti": ("I", 0x13, 2, None), "sltiu": ("I", 0x13, 3, None),
    "xori": ("I", 0x13, 4, None), "ori": ("I", 0x13, 6, None), "andi": ("I", 0x13, 7, None),
    "slli": ("SH", 0x13, 1, 0x00), "srli": ("SH", 0x13, 5, 0x00), "srai": ("SH", 0x13, 5, 0x20),
    "add": ("R", 0x33, 0, 0x00), "sub": ("R", 0x33, 0, 0x20), "sll": ("R", 0x33, 1, 0x00),
    "slt": ("R", 0x33, 2, 0x00), "sltu": ("R", 0x33, 3, 0x00), "xor": ("R", 0x33, 4, 0x00),
    "srl": ("R", 0x33, 5, 0x00), "sra": ("R", 0x33, 5, 0x20), "or": ("R", 0x33, 6, 0x00),
    "and": ("R", 0x33, 7, 0x00),
}
MNEMONICS = list(INSNS)  # the 37 RV32I "user" instructions (no fence/ecall/ebreak/csr)


def _chk(v, bits, signed, what):
    lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
    if not lo <= v <= hi:
        raise ValueError(f"{what} {v} out of range")


def encode(name, rd=0, rs1=0, rs2=0, imm=0):
    fmt, op, f3, f7 = INSNS[name]
    for r in (rd, rs1, rs2):
        _chk(r, 5, False, "register")
    if fmt == "R":
        return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op
    if fmt == "SH":
        _chk(imm, 5, False, "shift amount")
        return (f7 << 25) | (imm << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op
    if fmt == "I":
        _chk(imm, 12, True, "immediate")
        return ((imm & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op
    if fmt == "S":
        _chk(imm, 12, True, "immediate")
        i = imm & 0xFFF
        return ((i >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((i & 31) << 7) | op
    if fmt == "B":
        _chk(imm, 13, True, "branch offset")
        if imm & 1:
            raise ValueError("branch offset must be even")
        i = imm & 0x1FFF
        return (((i >> 12) & 1) << 31) | (((i >> 5) & 63) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | \
               (((i >> 1) & 15) << 8) | (((i >> 11) & 1) << 7) | op
    if fmt == "U":
        _chk(imm, 20, False, "upper immediate")
        return (imm << 12) | (rd << 7) | op
    if fmt == "J":
        _chk(imm, 21, True, "jump offset")
        i = imm & 0x1FFFFF
        return (((i >> 20) & 1) << 31) | (((i >> 1) & 0x3FF) << 21) | (((i >> 11) & 1) << 20) | \
               (((i >> 12) & 0xFF) << 12) | (rd << 7) | op
    raise ValueError(fmt)


def _sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def decode(w):
    op, rd, f3, rs1, rs2, f7 = w & 0x7F, (w >> 7) & 31, (w >> 12) & 7, (w >> 15) & 31, (w >> 20) & 31, w >> 25
    for name, (fmt, o, ff3, ff7) in INSNS.items():
        if o != op or (ff3 is not None and ff3 != f3):
            continue
        if fmt in ("R", "SH") and ff7 != f7:
            continue
        d = dict(name=name, rd=rd, rs1=rs1, rs2=rs2, imm=0, word=w)
        if fmt == "I":
            d["imm"] = _sext(w >> 20, 12)
        elif fmt == "SH":
            d["imm"] = rs2
        elif fmt == "S":
            d["imm"] = _sext(((w >> 25) << 5) | ((w >> 7) & 31), 12)
        elif fmt == "B":
            d["imm"] = _sext((((w >> 31) & 1) << 12) | (((w >> 7) & 1) << 11) | (((w >> 25) & 63) << 5) |
                             (((w >> 8) & 15) << 1), 13)
        elif fmt == "U":
            d["imm"] = w >> 12
        elif fmt == "J":
            d["imm"] = _sext((((w >> 31) & 1) << 20) | (((w >> 12) & 0xFF) << 12) | (((w >> 20) & 1) << 11) |
                             (((w >> 21) & 0x3FF) << 1), 21)
        return d
    return dict(name="illegal", rd=rd, rs1=rs1, rs2=rs2, imm=0, word=w)


def load_constant(rd, value):
    """lui + addi that put a 32-bit value into rd (two words)."""
    value &= 0xFFFFFFFF
    hi = ((value + 0x800) >> 12) & 0xFFFFF
    lo = _sext((value - (hi << 12)) & 0xFFF, 12)
    return [encode("lui", rd=rd, imm=hi), encode("addi", rd=rd, rs1=rd, imm=lo)]


EXIT_WORD = 0x80201073  # csrrw x0, 0x802, x0  -> exit(0)


def flat_image(body, init_regs, data):
    """Build the harness image. init_regs: 31 values for x1..x31 (x31 is overwritten with DATA_BASE)."""
    if len(body) > MAX_BODY:
        raise ValueError("body too long")
    if len(data) != DATA_SIZE:
        raise ValueError("data must be DATA_SIZE bytes")
    words = []
    for r in range(1, 31):
        words += load_constant(r, init_regs[r - 1])
    words += load_constant(BASE_REG, DATA_BASE)
    words += list(body)
    words.append(EXIT_WORD)
    code = b"".join((w & 0xFFFFFFFF).to_bytes(4, "little") for w in words)
    return code + bytes(DATA_OFF - len(code)) + bytes(data)


PROLOGUE_WORDS = 62  # 31 registers x 2 words: body starts at BASE + 4 * PROLOGUE_WORDS


def run_iss(cpu, image, golden=False, trace=None, max_instr=200000, timeout=20):
    """Run psp-lite on a flat image file. Returns (exit status, stdout, stderr)."""
    cmd = [cpu, "--flat", "--max-instr", str(max_instr)]
    if golden:
        cmd.append("--golden")
    if trace:
        cmd += ["--trace-file", trace]
    cmd.append(image)
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    return p.returncode, p.stdout.decode(errors="replace"), p.stderr.decode(errors="replace")


def parse_trace_line(line):
    """'   12 M 80000010 00a50533  add a0, a0, a0   a0=00000014' -> dict (or None for trap lines)."""
    parts = line.split()
    if len(parts) < 5:
        return None
    d = dict(n=int(parts[0]), priv=parts[1], pc=int(parts[2], 16), word=int(parts[3], 16), mnemonic=parts[4],
             line=line, trap="-> trap" in line, rd=None, rd_val=None, mem=None, mem_val=None)
    for tok in parts[5:]:
        if tok.startswith("mem[") and "]" in tok:
            addr, _, val = tok[4:].partition("]")
            d["mem"] = int(addr, 16)
            if val.startswith("="):
                d["mem_val"] = int(val[1:], 16)
        elif "=" in tok:
            k, _, v = tok.partition("=")
            if k in ABI and len(v) == 8:
                d["rd"], d["rd_val"] = ABI.index(k), int(v, 16)
    return d


def read_trace(path):
    with open(path) as f:
        return f.read().splitlines()


def find_cpu(default_rel="build/cpu/psp-lite"):
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(os.path.dirname(here), default_rel)
