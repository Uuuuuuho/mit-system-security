/* cpu.c - the psp-lite RV32I core: fetch/decode/execute, CSRs, traps.
 *
 * A plain switch-based interpreter (no decode cache), so self-modifying code
 * works without any fence.i.  Seeded defects live in defects.c and are reached
 * through four narrow hooks: alu_adder(), custom0_exec(), decode_fixup() and
 * x0_write_enable().
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "iss.h"

/* ------------------------------------------------------------------ helpers */
static inline uint32_t ld32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4); /* hosts (x86_64, aarch64) are little-endian like RISC-V */
    return v;
}
static inline uint16_t ld16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static inline void st32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void st16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

static inline int32_t imm_i(uint32_t i) { return (int32_t)i >> 20; }
static inline int32_t imm_s(uint32_t i) { return ((int32_t)i >> 25 << 5) | ((i >> 7) & 31); }
static inline int32_t imm_b(uint32_t i) {
    return ((int32_t)i >> 31 << 12) | ((i >> 7 & 1) << 11) | ((i >> 25 & 63) << 5) | ((i >> 8 & 15) << 1);
}
static inline int32_t imm_j(uint32_t i) {
    return ((int32_t)i >> 31 << 20) | (i & 0xFF000) | ((i >> 20 & 1) << 11) | ((i >> 21 & 0x3FF) << 1);
}

/* returns host pointer for [addr, addr+n) or NULL if outside RAM */
static inline uint8_t *mem_ptr(psp_cpu *c, uint32_t addr, uint32_t n) {
    uint32_t off = addr - c->ram_base;
    if (off >= c->ram_size || n > c->ram_size - off) return NULL;
    return c->ram + off;
}

const char *cause_name(uint32_t cause) {
    switch (cause) {
    case CAUSE_MISALIGNED_FETCH: return "instruction address misaligned";
    case CAUSE_FETCH_ACCESS: return "instruction access fault";
    case CAUSE_ILLEGAL_INSN: return "illegal instruction";
    case CAUSE_BREAKPOINT: return "breakpoint";
    case CAUSE_MISALIGNED_LOAD: return "load address misaligned";
    case CAUSE_LOAD_ACCESS: return "load access fault";
    case CAUSE_MISALIGNED_STORE: return "store address misaligned";
    case CAUSE_STORE_ACCESS: return "store access fault";
    case CAUSE_ECALL_U: return "ecall from U-mode";
    case CAUSE_ECALL_M: return "ecall from M-mode";
    case CAUSE_SOFTWARE_CHECK: return "software check (shadow stack)";
    default: return "?";
    }
}

void cpu_init(psp_cpu *c) {
    memset(c->x, 0, sizeof c->x);
    c->priv = PRV_M;
    c->mstatus = PRV_M << MSTATUS_MPP_SHIFT;
    c->mtvec = c->mepc = c->mcause = c->mtval = c->mscratch = c->mie = 0;
    c->cycle = c->instret = 0;
    c->halted = 0;
    c->exit_code = 0;
    c->in_eof = 0;
    ss_reset(&c->ss);
}

void cpu_dump_regs(const psp_cpu *c, FILE *f) {
    fprintf(f, "pc=%08x priv=%c instret=%" PRIu64 " mepc=%08x mcause=%08x mtval=%08x mstatus=%08x\n", c->pc,
            c->priv == PRV_M ? 'M' : 'U', c->instret, c->mepc, c->mcause, c->mtval, c->mstatus);
    for (int i = 0; i < 32; i++)
        fprintf(f, "x%-2d %-4s %08x%s", i, rv_abi_name[i], c->x[i], (i % 4 == 3) ? "\n" : "   ");
}

/* --------------------------------------------------------------- tracing */
static void trace_line(psp_cpu *c, uint32_t pc, uint32_t insn, uint32_t priv, const char *suffix) {
    char d[80];
    rv_disasm(insn, pc, d, sizeof d);
    fprintf(c->trace, "%10" PRIu64 " %c %08x %08x  %-36s", c->instret, priv == PRV_M ? 'M' : 'U', pc, insn, d);
    if (suffix) {
        fprintf(c->trace, " %s\n", suffix);
        return;
    }
    if (c->tr_rd >= 0) fprintf(c->trace, " %s=%08x", rv_abi_name[c->tr_rd], c->tr_rd_val);
    if (c->tr_mem == 1) fprintf(c->trace, " mem[%08x]", c->tr_mem_addr);
    if (c->tr_mem == 2) fprintf(c->trace, " mem[%08x]=%08x", c->tr_mem_addr, c->tr_mem_val);
    fputc('\n', c->trace);
}

/* ----------------------------------------------------------------- traps */
static void take_trap(psp_cpu *c, uint32_t cause, uint32_t tval, uint32_t insn) {
    if (c->trace) {
        char s[96];
        snprintf(s, sizeof s, "-> trap mcause=%u (%s) mtval=%08x", cause, cause_name(cause), tval);
        trace_line(c, c->pc, insn, c->priv, s);
    }
    uint32_t ms = c->mstatus & ~(MSTATUS_MPIE | MSTATUS_MPP | MSTATUS_MIE);
    if (c->mstatus & MSTATUS_MIE) ms |= MSTATUS_MPIE;
    ms |= c->priv << MSTATUS_MPP_SHIFT;
    c->mstatus = ms;
    c->mepc = c->pc;
    c->mcause = cause;
    c->mtval = tval;
    if ((c->mtvec & ~3u) == 0) { /* no handler installed: the simulator reports and stops */
        fflush(stdout);
        fprintf(stderr, "psp-lite: unhandled trap at pc=%08x (%c-mode): mcause=%u (%s), mtval=%08x\n", c->pc,
                c->priv == PRV_M ? 'M' : 'U', cause, cause_name(cause), tval);
        if (cause == CAUSE_SOFTWARE_CHECK)
            fprintf(stderr, "psp-lite: shadow stack violation: return target %08x, shadow stack expected %08x\n",
                    c->ss.fault_actual, c->ss.fault_expected);
        c->halted = 1;
        c->exit_code = EXIT_TRAP_BASE + (int)(cause & 63);
        return;
    }
    c->priv = PRV_M;
    c->pc = c->mtvec & ~3u;
}

/* ------------------------------------------------------------------ CSRs */
static int csr_read(psp_cpu *c, uint32_t csr, uint32_t *v) {
    switch (csr) {
    case CSR_MSTATUS: *v = c->mstatus; return 0;
    case CSR_MISA: *v = 0x40100100u; return 0; /* RV32 I U */
    case CSR_MIE: *v = c->mie; return 0;
    case CSR_MIP: *v = 0; return 0;
    case CSR_MTVEC: *v = c->mtvec; return 0;
    case CSR_MSCRATCH: *v = c->mscratch; return 0;
    case CSR_MEPC: *v = c->mepc; return 0;
    case CSR_MCAUSE: *v = c->mcause; return 0;
    case CSR_MTVAL: *v = c->mtval; return 0;
    case CSR_MCYCLE:
    case CSR_CYCLE:
    case CSR_TIME: *v = (uint32_t)c->cycle; return 0;
    case CSR_MCYCLEH:
    case CSR_CYCLEH:
    case CSR_TIMEH: *v = (uint32_t)(c->cycle >> 32); return 0;
    case CSR_MINSTRET:
    case CSR_INSTRET: *v = (uint32_t)c->instret; return 0;
    case CSR_MINSTRETH:
    case CSR_INSTRETH: *v = (uint32_t)(c->instret >> 32); return 0;
    case CSR_MVENDORID: *v = 0; return 0;
    case CSR_MARCHID: *v = 0x70737000u; return 0;
    case CSR_MIMPID: *v = 0x00010000u; return 0;
    case CSR_MHARTID: *v = 0; return 0;
    case CSR_PSP_SEROUT: *v = 0; return 0;
    case CSR_PSP_SERIN: {
        fflush(stdout); /* interactive use: show the prompt before blocking */
        int ch = c->in_eof ? EOF : fgetc(c->in);
        if (ch == EOF) c->in_eof = 1;
        *v = (ch == EOF) ? 0xFFFFFFFFu : (uint32_t)ch;
        return 0;
    }
    case CSR_PSP_EXIT: *v = 0; return 0;
    case CSR_PSP_LOADBASE: *v = c->load_base; return 0;
    default:
        if (csr >= CSR_PSP_FLAG0 && csr <= CSR_PSP_FLAG7) {
            *v = ld32(c->flag + 4 * (csr - CSR_PSP_FLAG0));
            return 0;
        }
        return -1;
    }
}

static void csr_write(psp_cpu *c, uint32_t csr, uint32_t v) {
    switch (csr) {
    case CSR_MSTATUS: {
        uint32_t mpp = (v >> MSTATUS_MPP_SHIFT) & 3;
        if (mpp != PRV_M) mpp = PRV_U; /* WARL: only U and M exist */
        c->mstatus = (v & (MSTATUS_MIE | MSTATUS_MPIE)) | (mpp << MSTATUS_MPP_SHIFT);
        break;
    }
    case CSR_MIE: c->mie = v; break;
    case CSR_MTVEC: c->mtvec = v & ~3u; break; /* direct mode only */
    case CSR_MSCRATCH: c->mscratch = v; break;
    case CSR_MEPC: c->mepc = v & ~3u; break;
    case CSR_MCAUSE: c->mcause = v; break;
    case CSR_MTVAL: c->mtval = v; break;
    case CSR_MCYCLE: c->cycle = (c->cycle & ~0xFFFFFFFFull) | v; break;
    case CSR_MCYCLEH: c->cycle = (c->cycle & 0xFFFFFFFFull) | ((uint64_t)v << 32); break;
    case CSR_MINSTRET: c->instret = (c->instret & ~0xFFFFFFFFull) | v; break;
    case CSR_MINSTRETH: c->instret = (c->instret & 0xFFFFFFFFull) | ((uint64_t)v << 32); break;
    case CSR_PSP_SEROUT: putchar((int)(v & 0xFF)); break;
    case CSR_PSP_EXIT:
        c->halted = 1;
        c->exit_code = (int)(v & 0xFF);
        break;
    default: break; /* WARL / read-only-zero registers ignore writes */
    }
}

/* ------------------------------------------------------------- the core */
/* A trapping instruction does not retire: jump past the retire step at the end of the loop body. */
#define ILLEGAL()                                                                                                  \
    do {                                                                                                           \
        take_trap(c, CAUSE_ILLEGAL_INSN, insn, insn);                                                              \
        goto trapped;                                                                                              \
    } while (0)
#define TRAP(cause, tval)                                                                                          \
    do {                                                                                                           \
        take_trap(c, (cause), (tval), insn);                                                                       \
        goto trapped;                                                                                              \
    } while (0)
#define WR(val)                                                                                                    \
    do {                                                                                                           \
        uint32_t v_ = (val);                                                                                       \
        if (rd)                                                                                                    \
            x[rd] = v_;                                                                                            \
        else if (x0_write_enable(c, insn))                                                                         \
            x[0] = v_;                                                                                             \
        c->tr_rd = (int)rd;                                                                                        \
        c->tr_rd_val = x[rd];                                                                                      \
    } while (0)

static inline int is_link(uint32_t r) { return r == 1 || r == 5; }

void cpu_run(psp_cpu *c) {
    uint32_t *x = c->x;
    while (!c->halted) {
        if (c->cycle >= c->max_steps) {
            fflush(stdout);
            fprintf(stderr, "psp-lite: instruction limit (%" PRIu64 ") reached at pc=%08x\n", c->max_steps, c->pc);
            c->halted = 1;
            c->exit_code = EXIT_MAX_INSTR;
            break;
        }
        c->cycle++;
        const uint32_t pc = c->pc;
        uint32_t insn = 0;
        uint8_t *ip = mem_ptr(c, pc, 4);
        if (!ip) TRAP(CAUSE_FETCH_ACCESS, pc);
        insn = ld32(ip);
        const uint32_t rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31;
        const uint32_t f3 = (insn >> 12) & 7;
        const uint32_t priv0 = c->priv;
        uint32_t next = pc + 4;
        c->tr_rd = -1;
        c->tr_mem = 0;

        switch (insn & 0x7f) {
        case 0x37: /* LUI */ WR(insn & 0xFFFFF000u); break;
        case 0x17: /* AUIPC */ WR(pc + (insn & 0xFFFFF000u)); break;
        case 0x6f: { /* JAL */
            uint32_t t = pc + (uint32_t)imm_j(insn);
            if (t & 3) TRAP(CAUSE_MISALIGNED_FETCH, t);
            if (c->shadow_stack && is_link(rd) && ss_on_call(&c->ss, pc, t, pc + 4) != SS_OK)
                TRAP(CAUSE_SOFTWARE_CHECK, 3);
            WR(pc + 4);
            next = t;
            break;
        }
        case 0x67: { /* JALR */
            if (f3) ILLEGAL();
            uint32_t t = (x[rs1] + (uint32_t)imm_i(insn)) & ~1u;
            if (t & 3) TRAP(CAUSE_MISALIGNED_FETCH, t);
            if (c->shadow_stack) {
                int rdl = is_link(rd), rsl = is_link(rs1);
                if (rsl && (!rdl || rd != rs1) && ss_on_return(&c->ss, pc, t) != SS_OK)
                    TRAP(CAUSE_SOFTWARE_CHECK, 3);
                if (rdl && ss_on_call(&c->ss, pc, t, pc + 4) != SS_OK) TRAP(CAUSE_SOFTWARE_CHECK, 3);
            }
            WR(pc + 4);
            next = t;
            break;
        }
        case 0x63: { /* BRANCH */
            uint32_t a = x[rs1], b = x[rs2];
            int take = 0;
            switch (f3) {
            case 0: take = a == b; break;
            case 1: take = a != b; break;
            case 4: take = (int32_t)a < (int32_t)b; break;
            case 5: take = (int32_t)a >= (int32_t)b; break;
            case 6: take = a < b; break;
            case 7: take = a >= b; break;
            default: ILLEGAL();
            }
            if (take) {
                uint32_t t = pc + (uint32_t)imm_b(insn);
                if (t & 3) TRAP(CAUSE_MISALIGNED_FETCH, t);
                next = t;
            }
            break;
        }
        case 0x03: { /* LOAD */
            uint32_t addr = x[rs1] + (uint32_t)imm_i(insn); /* address-generation adder, not the ALU */
            uint32_t size = (f3 & 3) == 0 ? 1 : (f3 & 3) == 1 ? 2 : 4;
            if (f3 == 3 || f3 > 5) ILLEGAL();
            if (addr & (size - 1)) TRAP(CAUSE_MISALIGNED_LOAD, addr);
            uint8_t *p = mem_ptr(c, addr, size);
            if (!p) TRAP(CAUSE_LOAD_ACCESS, addr);
            uint32_t v;
            switch (f3) {
            case 0: v = (uint32_t)(int32_t)(int8_t)p[0]; break;
            case 1: v = decode_fixup(c, insn, (uint32_t)(int32_t)(int16_t)ld16(p), 0); break;
            case 2: v = ld32(p); break;
            case 4: v = p[0]; break;
            default: v = ld16(p); break;
            }
            c->tr_mem = 1;
            c->tr_mem_addr = addr;
            WR(v);
            break;
        }
        case 0x23: { /* STORE */
            uint32_t addr = x[rs1] + (uint32_t)imm_s(insn);
            if (f3 > 2) ILLEGAL();
            uint32_t size = 1u << f3;
            if (addr & (size - 1)) TRAP(CAUSE_MISALIGNED_STORE, addr);
            uint8_t *p = mem_ptr(c, addr, size);
            if (!p) TRAP(CAUSE_STORE_ACCESS, addr);
            uint32_t v = x[rs2];
            if (f3 == 0)
                p[0] = (uint8_t)v;
            else if (f3 == 1)
                st16(p, (uint16_t)v);
            else
                st32(p, v);
            c->tr_mem = 2;
            c->tr_mem_addr = addr;
            c->tr_mem_val = f3 == 0 ? (v & 0xFF) : f3 == 1 ? (v & 0xFFFF) : v;
            break;
        }
        case 0x13: { /* OP-IMM */
            uint32_t a = x[rs1], imm = (uint32_t)imm_i(insn), sh = rs2, f7 = insn >> 25;
            switch (f3) {
            case 0: WR(alu_adder(c, a, imm)); break;
            case 2: WR((int32_t)a < (int32_t)imm); break;
            case 3: WR(decode_fixup(c, insn, a < imm, a)); break;
            case 4: WR(a ^ imm); break;
            case 6: WR(a | imm); break;
            case 7: WR(a & imm); break;
            case 1:
                if (f7) ILLEGAL();
                WR(a << sh);
                break;
            default: /* 5 */
                if (f7 == 0)
                    WR(a >> sh);
                else if (f7 == 0x20)
                    WR(decode_fixup(c, insn, (uint32_t)((int32_t)a >> sh), a));
                else
                    ILLEGAL();
                break;
            }
            break;
        }
        case 0x33: { /* OP */
            uint32_t a = x[rs1], b = x[rs2], f7 = insn >> 25;
            if (f7 == 0) {
                switch (f3) {
                case 0: WR(alu_adder(c, a, b)); break;
                case 1: WR(a << (b & 31)); break;
                case 2: WR((int32_t)a < (int32_t)b); break;
                case 3: WR(a < b); break;
                case 4: WR(a ^ b); break;
                case 5: WR(a >> (b & 31)); break;
                case 6: WR(a | b); break;
                default: WR(a & b); break;
                }
            } else if (f7 == 0x20 && f3 == 0) {
                WR(a - b);
            } else if (f7 == 0x20 && f3 == 5) {
                WR((uint32_t)((int32_t)a >> (b & 31)));
            } else {
                ILLEGAL();
            }
            break;
        }
        case 0x0f: /* MISC-MEM: fence / fence.i are no-ops (no caches, in-order) */
            if (f3 > 1) ILLEGAL();
            break;
        case 0x0b: { /* custom-0 */
            uint32_t v;
            int r = custom0_exec(c, insn, &v);
            if (r < 0) ILLEGAL();
            if (r > 0) WR(v);
            break;
        }
        case 0x73: { /* SYSTEM */
            if (f3 == 0) {
                if (insn == 0x00000073) TRAP(priv0 == PRV_M ? CAUSE_ECALL_M : CAUSE_ECALL_U, 0);
                if (insn == 0x00100073) TRAP(CAUSE_BREAKPOINT, pc);
                if (insn == 0x10500073) break; /* wfi: no interrupts, so a no-op */
                if (insn == 0x30200073 && priv0 == PRV_M) { /* mret */
                    uint32_t ms = c->mstatus;
                    c->priv = (ms >> MSTATUS_MPP_SHIFT) & 3;
                    ms = (ms & ~(MSTATUS_MIE | MSTATUS_MPP)) | ((ms & MSTATUS_MPIE) ? MSTATUS_MIE : 0) | MSTATUS_MPIE;
                    c->mstatus = ms; /* MPP <- U */
                    next = c->mepc;
                    break;
                }
                ILLEGAL();
            }
            if (f3 == 4) ILLEGAL();
            uint32_t csr = insn >> 20, op = f3 & 3;
            uint32_t src = (f3 & 4) ? rs1 : x[rs1];
            int writes = (op == 1) || rs1 != 0;
            int reads = !(op == 1 && rd == 0);
            if (((csr >> 8) & 3) > priv0) ILLEGAL();          /* privilege encoded in the address */
            if (writes && (csr >> 10) == 3) ILLEGAL();         /* read-only CSR */
            uint32_t old = 0;
            if (reads || op != 1) {
                if (csr_read(c, csr, &old) < 0) ILLEGAL();
            } else {
                uint32_t dummy; /* CSRRW x0: still must be an existing CSR, but no read side effect */
                if (csr == CSR_PSP_SERIN)
                    dummy = 0;
                else if (csr_read(c, csr, &dummy) < 0)
                    ILLEGAL();
                (void)dummy;
            }
            if (writes) csr_write(c, csr, op == 1 ? src : op == 2 ? (old | src) : (old & ~src));
            WR(old);
            break;
        }
        default: ILLEGAL();
        }
        if (c->trace) trace_line(c, pc, insn, priv0, NULL);
        c->instret++;
        c->pc = next;
        continue;
    trapped:; /* take_trap() already redirected pc (or halted the simulator) */
    }
}
