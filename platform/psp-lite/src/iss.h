/* iss.h - internal interface of the psp-lite RV32I instruction-set simulator.
 *
 * psp-lite models a tiny "platform security processor": one RV32I hart with
 * machine (M) and user (U) privilege, a handful of CSRs, flat RAM and a serial
 * port that is reached through custom CSRs.  See ../README.md for the
 * programmer's model (memory map, CSR table, trap causes, command line).
 */
#ifndef PSP_ISS_H
#define PSP_ISS_H

#include <stdint.h>
#include <stdio.h>

#define PSP_VERSION "1.0"

#ifndef PSP_DEFECTS
#define PSP_DEFECTS 1 /* build with DEFECTS=0 to compile the seeded defects out */
#endif
#ifndef PSP_SS_SRC_NAME
#define PSP_SS_SRC_NAME "src/shadow_stack.c"
#endif

/* ---- privilege levels ---- */
#define PRV_U 0u
#define PRV_M 3u

/* ---- memory map ---- */
#define RAM_BASE_DEFAULT 0x80000000u
#define RAM_MB_DEFAULT 64u
#define ASLR_MAX_PAGES 8192u /* slide is 4 KiB * [0, 8192) -> up to 32 MiB, 13 bits */

/* ---- trap causes (mcause) ---- */
#define CAUSE_MISALIGNED_FETCH 0u
#define CAUSE_FETCH_ACCESS 1u
#define CAUSE_ILLEGAL_INSN 2u
#define CAUSE_BREAKPOINT 3u
#define CAUSE_MISALIGNED_LOAD 4u
#define CAUSE_LOAD_ACCESS 5u
#define CAUSE_MISALIGNED_STORE 6u
#define CAUSE_STORE_ACCESS 7u
#define CAUSE_ECALL_U 8u
#define CAUSE_ECALL_M 11u
#define CAUSE_SOFTWARE_CHECK 18u /* used for shadow-stack faults, mtval = 3 (as in Zicfiss) */

/* ---- CSR addresses ---- */
#define CSR_MSTATUS 0x300
#define CSR_MISA 0x301
#define CSR_MIE 0x304
#define CSR_MTVEC 0x305
#define CSR_MSCRATCH 0x340
#define CSR_MEPC 0x341
#define CSR_MCAUSE 0x342
#define CSR_MTVAL 0x343
#define CSR_MIP 0x344
#define CSR_MCYCLE 0xB00
#define CSR_MINSTRET 0xB02
#define CSR_MCYCLEH 0xB80
#define CSR_MINSTRETH 0xB82
#define CSR_CYCLE 0xC00
#define CSR_TIME 0xC01
#define CSR_INSTRET 0xC02
#define CSR_CYCLEH 0xC80
#define CSR_TIMEH 0xC81
#define CSR_INSTRETH 0xC82
#define CSR_MVENDORID 0xF11
#define CSR_MARCHID 0xF12
#define CSR_MIMPID 0xF13
#define CSR_MHARTID 0xF14
/* psp-lite custom CSRs */
#define CSR_PSP_SEROUT 0x800   /* U  RW : write -> one byte to the serial port (stdout)        */
#define CSR_PSP_SERIN 0x801    /* U  RW : read  -> next input byte, 0xFFFFFFFF at end of input */
#define CSR_PSP_EXIT 0x802     /* U  RW : write -> halt the simulator with this exit code      */
#define CSR_PSP_LOADBASE 0xCC0 /* U  RO : address the image was loaded at ("debug" register)   */
#define CSR_PSP_FLAG0 0xFC0    /* M  RO : 0xFC0..0xFC7 hold a 32-byte flag string              */
#define CSR_PSP_FLAG7 0xFC7

/* mstatus fields we model */
#define MSTATUS_MIE (1u << 3)
#define MSTATUS_MPIE (1u << 7)
#define MSTATUS_MPP_SHIFT 11
#define MSTATUS_MPP (3u << MSTATUS_MPP_SHIFT)

/* exit status conventions of the psp-lite process */
#define EXIT_LOAD_ERROR 2
#define EXIT_TRAP_BASE 64 /* unhandled trap -> 64 + mcause */
#define EXIT_MAX_INSTR 124

#include "shadow_stack.h"

typedef struct psp_cpu {
    uint32_t x[32];
    uint32_t pc;
    uint32_t priv;

    /* CSRs */
    uint32_t mstatus, mtvec, mepc, mcause, mtval, mscratch, mie;
    uint64_t cycle;   /* every executed step, including trapping ones */
    uint64_t instret; /* retired instructions */

    /* memory */
    uint8_t *ram;
    uint32_t ram_base, ram_size;
    uint32_t load_base; /* where the image was placed (CSR 0xCC0) */

    /* configuration */
    int golden;       /* 1: all seeded defects disabled */
    int shadow_stack; /* 1: call the shadow-stack hooks (optional exercise) */
    uint64_t max_steps;
    FILE *trace; /* NULL = tracing off */
    FILE *in;    /* serial input */
    uint8_t flag[32];

    /* run state */
    int halted;
    int exit_code;
    int in_eof;

    /* per-step trace scratch (filled by the executor when tracing) */
    int tr_rd;
    uint32_t tr_rd_val;
    int tr_mem;
    uint32_t tr_mem_addr, tr_mem_val;

    psp_shadow_stack ss;
} psp_cpu;

/* cpu.c */
void cpu_init(psp_cpu *c);
void cpu_run(psp_cpu *c);
void cpu_dump_regs(const psp_cpu *c, FILE *f);
const char *cause_name(uint32_t cause);

/* disasm.c */
void rv_disasm(uint32_t insn, uint32_t pc, char *buf, size_t n);
const char *rv_mnemonic(uint32_t insn);
extern const char *const rv_abi_name[32];

/* loader.c : returns 0 on success; *span_out = bytes of RAM the image needs */
int load_image(psp_cpu *c, const char *path, int force_flat, uint32_t base, uint32_t *entry_out);
int image_span(const char *path, int force_flat, uint32_t *span_out, int *is_elf);

/* defects.c : seeded defects ("errata").  All return the golden result when c->golden. */
void defects_init(void);
/* the ALU adder used by ADD and ADDI (address generation and PC arithmetic use their own adders) */
uint32_t alu_adder(const psp_cpu *c, uint32_t a, uint32_t b);
/* custom-0 opcode space: returns -1 = illegal (trap), 0 = executed, 1 = executed and writes *rd_val */
int custom0_exec(psp_cpu *c, uint32_t insn, uint32_t *rd_val);
/* decoder fix-up hook for a few instruction classes (SRAI, SLTIU, LH): returns the value to write */
uint32_t decode_fixup(const psp_cpu *c, uint32_t insn, uint32_t golden_result, uint32_t aux);
/* register-file write enable for rd == x0 (1 = the write happens) */
int x0_write_enable(const psp_cpu *c, uint32_t insn);

#endif
