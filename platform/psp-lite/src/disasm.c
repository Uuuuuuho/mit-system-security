/* disasm.c - a small, always-correct RV32I disassembler used by --trace.
 * It decodes the instruction word on its own (it does not ask the core), so a
 * trace shows what the instruction *should* be even on a defective CPU. */
#include <stdio.h>
#include <string.h>

#include "iss.h"

const char *const rv_abi_name[32] = {"zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
                                     "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
                                     "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};

static int32_t imm_i(uint32_t i) { return (int32_t)i >> 20; }
static int32_t imm_s(uint32_t i) { return ((int32_t)i >> 25 << 5) | ((i >> 7) & 31); }
static int32_t imm_b(uint32_t i) {
    return ((int32_t)i >> 31 << 12) | ((i >> 7 & 1) << 11) | ((i >> 25 & 63) << 5) | ((i >> 8 & 15) << 1);
}
static int32_t imm_j(uint32_t i) {
    return ((int32_t)i >> 31 << 20) | (i & 0xFF000) | ((i >> 20 & 1) << 11) | ((i >> 21 & 0x3FF) << 1);
}

const char *rv_mnemonic(uint32_t insn) {
    uint32_t op = insn & 0x7f, f3 = (insn >> 12) & 7, f7 = insn >> 25;
    switch (op) {
    case 0x37: return "lui";
    case 0x17: return "auipc";
    case 0x6f: return "jal";
    case 0x67: return f3 == 0 ? "jalr" : "illegal";
    case 0x63: {
        static const char *b[8] = {"beq", "bne", "illegal", "illegal", "blt", "bge", "bltu", "bgeu"};
        return b[f3];
    }
    case 0x03: {
        static const char *l[8] = {"lb", "lh", "lw", "illegal", "lbu", "lhu", "illegal", "illegal"};
        return l[f3];
    }
    case 0x23: {
        static const char *s[8] = {"sb", "sh", "sw", "illegal", "illegal", "illegal", "illegal", "illegal"};
        return s[f3];
    }
    case 0x13:
        switch (f3) {
        case 0: return "addi";
        case 1: return f7 == 0 ? "slli" : "illegal";
        case 2: return "slti";
        case 3: return "sltiu";
        case 4: return "xori";
        case 5: return f7 == 0 ? "srli" : f7 == 0x20 ? "srai" : "illegal";
        case 6: return "ori";
        default: return "andi";
        }
    case 0x33:
        if (f7 == 0) {
            static const char *r[8] = {"add", "sll", "slt", "sltu", "xor", "srl", "or", "and"};
            return r[f3];
        }
        if (f7 == 0x20 && f3 == 0) return "sub";
        if (f7 == 0x20 && f3 == 5) return "sra";
        return "illegal";
    case 0x0f: return f3 == 0 ? "fence" : f3 == 1 ? "fence.i" : "illegal";
    case 0x73:
        switch (f3) {
        case 0:
            if (insn == 0x00000073) return "ecall";
            if (insn == 0x00100073) return "ebreak";
            if (insn == 0x30200073) return "mret";
            if (insn == 0x10500073) return "wfi";
            return "illegal";
        case 1: return "csrrw";
        case 2: return "csrrs";
        case 3: return "csrrc";
        case 5: return "csrrwi";
        case 6: return "csrrsi";
        case 7: return "csrrci";
        default: return "illegal";
        }
    case 0x0b: return (f3 == 0 && f7 == 0) ? "psp.id" : "custom0";
    default: return "illegal";
    }
}

void rv_disasm(uint32_t insn, uint32_t pc, char *buf, size_t n) {
    const char *m = rv_mnemonic(insn);
    uint32_t op = insn & 0x7f, f3 = (insn >> 12) & 7;
    const char *rd = rv_abi_name[(insn >> 7) & 31], *rs1 = rv_abi_name[(insn >> 15) & 31],
               *rs2 = rv_abi_name[(insn >> 20) & 31];
    if (!strcmp(m, "illegal")) {
        snprintf(buf, n, ".word 0x%08x", insn);
        return;
    }
    switch (op) {
    case 0x37:
    case 0x17: snprintf(buf, n, "%-7s %s, 0x%x", m, rd, insn >> 12); break;
    case 0x6f: snprintf(buf, n, "%-7s %s, 0x%08x", m, rd, pc + (uint32_t)imm_j(insn)); break;
    case 0x67: snprintf(buf, n, "%-7s %s, %d(%s)", m, rd, imm_i(insn), rs1); break;
    case 0x63: snprintf(buf, n, "%-7s %s, %s, 0x%08x", m, rs1, rs2, pc + (uint32_t)imm_b(insn)); break;
    case 0x03: snprintf(buf, n, "%-7s %s, %d(%s)", m, rd, imm_i(insn), rs1); break;
    case 0x23: snprintf(buf, n, "%-7s %s, %d(%s)", m, rs2, imm_s(insn), rs1); break;
    case 0x13:
        if (f3 == 1 || f3 == 5)
            snprintf(buf, n, "%-7s %s, %s, %u", m, rd, rs1, (insn >> 20) & 31);
        else
            snprintf(buf, n, "%-7s %s, %s, %d", m, rd, rs1, imm_i(insn));
        break;
    case 0x33: snprintf(buf, n, "%-7s %s, %s, %s", m, rd, rs1, rs2); break;
    case 0x0f: snprintf(buf, n, "%s", m); break;
    case 0x73:
        if (f3 == 0)
            snprintf(buf, n, "%s", m);
        else if (f3 & 4)
            snprintf(buf, n, "%-7s %s, 0x%03x, %u", m, rd, insn >> 20, (insn >> 15) & 31);
        else
            snprintf(buf, n, "%-7s %s, 0x%03x, %s", m, rd, insn >> 20, rs1);
        break;
    case 0x0b:
        if (f3 == 0 && (insn >> 25) == 0)
            snprintf(buf, n, "%-7s %s", m, rd);
        else
            snprintf(buf, n, "%-7s f3=%u f7=0x%02x %s, %s, %s", m, f3, insn >> 25, rd, rs1, rs2);
        break;
    default: snprintf(buf, n, ".word 0x%08x", insn); break;
    }
}
