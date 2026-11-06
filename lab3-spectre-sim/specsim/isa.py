"""specsim ISA: registers, opcodes and instruction semantics.

The toy ISA is a 64-bit load/store RISC machine that borrows RISC-V register names
so the assembly looks familiar.  Every instruction is 4 bytes in the (separate)
instruction memory.  See README.md, section "ISA reference".

A decoded instruction is a plain tuple, which keeps the simulator loop fast:

    (op, rd, rs1, rs2, imm, fn, lat)

    op   opcode number (constants below)
    rd   destination register (0 if none)
    rs1  first source register (0 = zero register if unused)
    rs2  second source register (0 if unused)
    imm  immediate / memory offset / branch-or-jump target PC
    fn   ALU or branch-condition function, or the access size for memory ops
    lat  execution latency in cycles for ALU ops (unused otherwise)
"""

M64 = (1 << 64) - 1

REG_NAMES = [
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
]
REGS = {name: i for i, name in enumerate(REG_NAMES)}
REGS.update({f"x{i}": i for i in range(32)})
REGS["fp"] = 8


def _signed(v):
    return v - (1 << 64) if v >> 63 else v


def _sra(a, b):
    return (_signed(a) >> (b & 63)) & M64


def _divu(a, b):
    return M64 if b == 0 else a // b


def _remu(a, b):
    return a if b == 0 else a % b


# ---------------------------------------------------------------------------
# Opcodes.  The numeric order matters: the simulator tests ranges.
# ---------------------------------------------------------------------------
# ALU, register-register
ADD, SUB, AND, OR, XOR, SLL, SRL, SRA, MUL, DIVU, REMU, SLTU, SLT = range(13)
RR_LAST = SLT
# ALU, register-immediate (LI is "rd = imm")
ADDI, ANDI, ORI, XORI, SLLI, SRLI, SRAI, SLTIU, LI = range(13, 22)
ALU_LAST = LI
# memory
LD, LW, LB = range(22, 25)
LOAD_LAST = LB
SD, SW, SB = range(25, 28)
STORE_LAST = SB
# control flow
BEQ, BNE, BLT, BGE, BLTU, BGEU = range(28, 34)
BRANCH_LAST = BGEU
JAL, JALR = 34, 35
# system / timing
FENCE, RDCYCLE, CLFLUSH, SYSCALL, SYSRET, HALT, NOP = range(36, 43)

ALU_FN = {
    ADD: lambda a, b: (a + b) & M64,
    SUB: lambda a, b: (a - b) & M64,
    AND: lambda a, b: a & b,
    OR: lambda a, b: a | b,
    XOR: lambda a, b: a ^ b,
    SLL: lambda a, b: (a << (b & 63)) & M64,
    SRL: lambda a, b: a >> (b & 63),
    SRA: _sra,
    MUL: lambda a, b: (a * b) & M64,
    DIVU: _divu,
    REMU: _remu,
    SLTU: lambda a, b: 1 if a < b else 0,
    SLT: lambda a, b: 1 if _signed(a) < _signed(b) else 0,
}
for _ri, _rr in ((ADDI, ADD), (ANDI, AND), (ORI, OR), (XORI, XOR),
                 (SLLI, SLL), (SRLI, SRL), (SRAI, SRA), (SLTIU, SLTU)):
    ALU_FN[_ri] = ALU_FN[_rr]
ALU_FN[LI] = lambda a, b: b

ALU_LAT = {op: 1 for op in ALU_FN}
ALU_LAT[MUL] = 4
ALU_LAT[DIVU] = 16
ALU_LAT[REMU] = 16

BRANCH_FN = {
    BEQ: lambda a, b: a == b,
    BNE: lambda a, b: a != b,
    BLT: lambda a, b: _signed(a) < _signed(b),
    BGE: lambda a, b: _signed(a) >= _signed(b),
    BLTU: lambda a, b: a < b,
    BGEU: lambda a, b: a >= b,
}

MEM_SIZE = {LD: 8, LW: 4, LB: 1, SD: 8, SW: 4, SB: 1}

# mnemonic -> opcode, operand format
#   R: rd, rs1, rs2        I: rd, rs1, imm      LI: rd, imm
#   L: rd, off(rs1)        S: rs2, off(rs1)     B: rs1, rs2, label
#   JAL: [rd,] label       JALR: rd, rs1[, imm] or rd, off(rs1)
#   C: off(rs1) (clflush)  RD: rd (rdcycle)     N: no operands
MNEMONICS = {
    "add": (ADD, "R"), "sub": (SUB, "R"), "and": (AND, "R"), "or": (OR, "R"),
    "xor": (XOR, "R"), "sll": (SLL, "R"), "srl": (SRL, "R"), "sra": (SRA, "R"),
    "mul": (MUL, "R"), "divu": (DIVU, "R"), "remu": (REMU, "R"),
    "sltu": (SLTU, "R"), "slt": (SLT, "R"),
    "addi": (ADDI, "I"), "andi": (ANDI, "I"), "ori": (ORI, "I"), "xori": (XORI, "I"),
    "slli": (SLLI, "I"), "srli": (SRLI, "I"), "srai": (SRAI, "I"), "sltiu": (SLTIU, "I"),
    "li": (LI, "LI"), "la": (LI, "LI"),
    "ld": (LD, "L"), "lw": (LW, "L"), "lb": (LB, "L"),
    "sd": (SD, "S"), "sw": (SW, "S"), "sb": (SB, "S"),
    "beq": (BEQ, "B"), "bne": (BNE, "B"), "blt": (BLT, "B"), "bge": (BGE, "B"),
    "bltu": (BLTU, "B"), "bgeu": (BGEU, "B"),
    "jal": (JAL, "JAL"), "jalr": (JALR, "JALR"),
    "fence": (FENCE, "N"), "rdcycle": (RDCYCLE, "RD"), "clflush": (CLFLUSH, "C"),
    "syscall": (SYSCALL, "N"), "sysret": (SYSRET, "N"), "halt": (HALT, "N"),
    "nop": (NOP, "N"),
}

# Pseudo-instructions, expanded by the assembler into exactly one real instruction.
#   mv rd, rs        -> addi rd, rs, 0
#   not rd, rs       -> xori rd, rs, -1
#   neg rd, rs       -> sub rd, zero, rs
#   beqz/bnez rs, L  -> beq/bne rs, zero, L
#   bgt/ble/bgtu/bleu a, b, L -> blt/bge/bltu/bgeu b, a, L
#   j L              -> jal zero, L
#   call L           -> jal ra, L
#   jr rs            -> jalr zero, rs, 0
#   ret              -> jalr zero, ra, 0
PSEUDO = {"mv", "not", "neg", "beqz", "bnez", "bgt", "ble", "bgtu", "bleu",
          "j", "call", "jr", "ret"}

COND_BRANCH_MNEMONICS = {"beq", "bne", "blt", "bge", "bltu", "bgeu"}
LOAD_MNEMONICS = {"ld", "lw", "lb"}
STORE_MNEMONICS = {"sd", "sw", "sb"}
