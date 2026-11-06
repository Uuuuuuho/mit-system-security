/* isa_test.c - self-test of the RV32I semantics psp-lite implements.
 *
 * Every check runs a real instruction through inline assembly and compares the
 * result with a value worked out by hand from the RISC-V specification.  Run it
 * with --golden: on a defective build some checks may (correctly!) fail.
 * Prints "isa_test: N/N passed" and exits 0 when everything matches.
 */
#include "psp.h"

static int total, fails;

static void check(const char *what, int line, uint32_t got, uint32_t exp) {
    total++;
    if (got != exp) {
        fails++;
        printf("FAIL %-8s line %d: got %08x expected %08x\n", what, line, got, exp);
    }
}

#define RR(op, a, b, e)                                                                                            \
    do {                                                                                                           \
        uint32_t r_;                                                                                               \
        __asm__ volatile(op " %0, %1, %2" : "=r"(r_) : "r"((uint32_t)(a)), "r"((uint32_t)(b)));                   \
        check(op, __LINE__, r_, (uint32_t)(e));                                                                    \
    } while (0)
#define RI(op, a, i, e)                                                                                            \
    do {                                                                                                           \
        uint32_t r_;                                                                                               \
        __asm__ volatile(op " %0, %1, %2" : "=r"(r_) : "r"((uint32_t)(a)), "i"(i));                               \
        check(op, __LINE__, r_, (uint32_t)(e));                                                                    \
    } while (0)
#define BR(op, a, b, taken)                                                                                        \
    do {                                                                                                           \
        uint32_t r_;                                                                                               \
        __asm__ volatile("li %0, 0\n\t" op " %1, %2, 1f\n\tj 2f\n1:\tli %0, 1\n2:"                              \
                         : "=&r"(r_)                                                                               \
                         : "r"((uint32_t)(a)), "r"((uint32_t)(b)));                                                \
        check(op, __LINE__, r_, (taken));                                                                          \
    } while (0)

/* ------------------------------------------------------------- traps */
static volatile uint32_t t_cause, t_tval, t_epc, t_count, t_recover;

__attribute__((interrupt("machine"))) static void handler(void) {
    t_cause = csr_read(mcause);
    t_tval = csr_read(mtval);
    t_epc = csr_read(mepc);
    t_count++;
    if (t_cause == MCAUSE_ECALL_U) { /* the user-mode part is over */
        printf("isa_test: %d/%d passed\n", total - fails, total);
        psp_exit(fails ? 1 : 0);
    }
    if (t_cause == 1) /* instruction access fault: mepc is the bad target, resume at t_recover */
        csr_write(mepc, t_recover);
    else
        csr_write(mepc, t_epc + 4);
}

#define TRAPS(code, cause, tval_expr)                                                                              \
    do {                                                                                                           \
        uint32_t before_ = t_count;                                                                                \
        __asm__ volatile(code ::: "memory", "t0", "t1", "t2");                                                    \
        check("trapcnt", __LINE__, t_count - before_, 1);                                                          \
        check("mcause", __LINE__, t_cause, (cause));                                                               \
        if ((tval_expr) != 0xDEADu) check("mtval", __LINE__, t_tval, (tval_expr));                                \
    } while (0)

static uint8_t mem[16] __attribute__((aligned(16))) = {0x01, 0x82, 0x03, 0x84, 0xF5, 0x06, 0x77, 0x88,
                                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static void user_part(void) {
    uint32_t r;
    /* reading a user CSR is fine, an M-mode CSR traps */
    r = csr_read(cycle);
    check("cycle-u", __LINE__, r != 0, 1);
    TRAPS("csrr t0, mstatus", MCAUSE_ILLEGAL_INSN, 0x300022f3u);
    TRAPS("csrr t0, 0xFC0", MCAUSE_ILLEGAL_INSN, 0xFC0022f3u);
    TRAPS("mret", MCAUSE_ILLEGAL_INSN, 0x30200073u);
    __asm__ volatile(".insn r 0x0b, 0, 0, %0, x0, x0" : "=r"(r)); /* psp.id works in U-mode too */
    check("psp.id-u", __LINE__, r, 0x50535031u);
    __asm__ volatile("ecall"); /* -> handler prints the summary and exits */
    for (;;) {
    }
}

int main(void) {
    uint32_t r, r2;
    /* ---------------- register-register ---------------- */
    RR("add", 1, 2, 3);
    RR("add", 0xFFFFFFFFu, 1, 0);
    RR("add", 0x7FFFFFFFu, 1, 0x80000000u);
    RR("add", 0x12345678u, 0x11111111u, 0x23456789u);
    RR("sub", 3, 5, 0xFFFFFFFEu);
    RR("sub", 0, 1, 0xFFFFFFFFu);
    RR("sub", 0x80000000u, 1, 0x7FFFFFFFu);
    RR("sll", 1, 31, 0x80000000u);
    RR("sll", 1, 32, 1);
    RR("sll", 0x12345678u, 0x24, 0x23456780u);
    RR("srl", 0x80000000u, 31, 1);
    RR("srl", 0xF0000000u, 4, 0x0F000000u);
    RR("srl", 0x80000000u, 33, 0x40000000u);
    RR("sra", 0x80000000u, 31, 0xFFFFFFFFu);
    RR("sra", 0xF0000000u, 4, 0xFF000000u);
    RR("sra", 0x70000000u, 4, 0x07000000u);
    RR("sra", 0x80000000u, 32, 0x80000000u);
    RR("slt", 0xFFFFFFFFu, 0, 1);
    RR("slt", 0, 0xFFFFFFFFu, 0);
    RR("slt", 5, 5, 0);
    RR("slt", 0x80000000u, 0x7FFFFFFFu, 1);
    RR("sltu", 0xFFFFFFFFu, 0, 0);
    RR("sltu", 0, 0xFFFFFFFFu, 1);
    RR("sltu", 5, 5, 0);
    RR("xor", 0xFF00FF00u, 0x0FF00FF0u, 0xF0F0F0F0u);
    RR("or", 0xFF00FF00u, 0x0FF00FF0u, 0xFFF0FFF0u);
    RR("and", 0xFF00FF00u, 0x0FF00FF0u, 0x0F000F00u);

    /* ---------------- register-immediate ---------------- */
    RI("addi", 1, -1, 0);
    RI("addi", 0x7FFFFFFFu, 1, 0x80000000u);
    RI("addi", 0, 2047, 0x7FFu);
    RI("addi", 0, -2048, 0xFFFFF800u);
    RI("slti", 0xFFFFFFFBu, -4, 1);
    RI("slti", 0xFFFFFFFCu, -5, 0);
    RI("slti", 0, -1, 0);
    RI("sltiu", 0, -1, 1);
    RI("sltiu", 0xFFFFFFFEu, -1, 1);
    RI("sltiu", 0xFFFFFFFFu, -1, 0);
    RI("sltiu", 5, 3, 0);
    RI("sltiu", 2, 3, 1);
    RI("sltiu", 0x800u, -2048, 1);
    RI("xori", 0x00FF00FFu, -1, 0xFF00FF00u);
    RI("xori", 0x0F, 0x0F0, 0xFF);
    RI("ori", 0xF0, 0x70F, 0x7FF);
    RI("ori", 0, -2048, 0xFFFFF800u);
    RI("andi", 0xFFFFFFFFu, 0x7FF, 0x7FF);
    RI("andi", 0x12345678u, -16, 0x12345670u);
    RI("slli", 1, 31, 0x80000000u);
    RI("slli", 0x12345678u, 8, 0x34567800u);
    RI("srli", 0x80000000u, 31, 1);
    RI("srli", 0x87654321u, 16, 0x8765u);
    RI("srai", 0x87654321u, 16, 0xFFFF8765u);
    RI("srai", 0x80000000u, 1, 0xC0000000u);
    RI("srai", 0x7FFFFFFFu, 30, 1);
    RI("srai", 0x80000000u, 31, 0xFFFFFFFFu);

    /* ---------------- upper immediates ---------------- */
    __asm__ volatile("lui %0, 0x12345" : "=r"(r));
    check("lui", __LINE__, r, 0x12345000u);
    __asm__ volatile("lui %0, 0xFFFFF" : "=r"(r));
    check("lui", __LINE__, r, 0xFFFFF000u);
    __asm__ volatile("1: auipc %0, 0\n\tlla %1, 1b" : "=r"(r), "=r"(r2));
    check("auipc", __LINE__, r, r2);
    __asm__ volatile("1: auipc %0, 1\n\tlla %1, 1b" : "=r"(r), "=r"(r2));
    check("auipc", __LINE__, r - r2, 0x1000u);
    /* x0 stays zero */
    __asm__ volatile("addi x0, x0, 5\n\tlui x0, 0x12345\n\tauipc x0, 7\n\tmv %0, x0" : "=r"(r));
    check("x0", __LINE__, r, 0);

    /* ---------------- loads and stores ---------------- */
    uint8_t *m = mem;
    __asm__ volatile("lb %0, 1(%1)" : "=r"(r) : "r"(m));
    check("lb", __LINE__, r, 0xFFFFFF82u);
    __asm__ volatile("lbu %0, 1(%1)" : "=r"(r) : "r"(m));
    check("lbu", __LINE__, r, 0x82u);
    __asm__ volatile("lh %0, 0(%1)" : "=r"(r) : "r"(m));
    check("lh", __LINE__, r, 0xFFFF8201u);
    __asm__ volatile("lh %0, -2(%1)" : "=r"(r) : "r"(m + 4));
    check("lh", __LINE__, r, 0xFFFF8403u);
    __asm__ volatile("lhu %0, 2(%1)" : "=r"(r) : "r"(m));
    check("lhu", __LINE__, r, 0x8403u);
    __asm__ volatile("lw %0, 4(%1)" : "=r"(r) : "r"(m));
    check("lw", __LINE__, r, 0x887706F5u);
    __asm__ volatile("lw %0, -4(%1)" : "=r"(r) : "r"(m + 8));
    check("lw", __LINE__, r, 0x887706F5u);
    __asm__ volatile("sw %1, 8(%0)\n\tsb %2, 9(%0)\n\tsh %2, 14(%0)" ::"r"(m), "r"(0x11223344u), "r"(0xA5B6C7D8u)
                     : "memory");
    __asm__ volatile("lw %0, 8(%1)" : "=r"(r) : "r"(m));
    check("sb", __LINE__, r, 0x1122D844u);
    __asm__ volatile("lw %0, 12(%1)" : "=r"(r) : "r"(m));
    check("sh", __LINE__, r, 0xC7D80000u);

    /* ---------------- branches ---------------- */
    BR("beq", 5, 5, 1);
    BR("beq", 5, 6, 0);
    BR("bne", 5, 6, 1);
    BR("bne", 5, 5, 0);
    BR("blt", 0xFFFFFFFFu, 0, 1);
    BR("blt", 0, 0xFFFFFFFFu, 0);
    BR("bge", 0, 0xFFFFFFFFu, 1);
    BR("bge", 3, 3, 1);
    BR("bge", 0xFFFFFFFFu, 0, 0);
    BR("bltu", 0, 0xFFFFFFFFu, 1);
    BR("bltu", 0xFFFFFFFFu, 0, 0);
    BR("bgeu", 0xFFFFFFFFu, 0, 1);
    BR("bgeu", 0, 0xFFFFFFFFu, 0);
    BR("bgeu", 7, 7, 1);

    /* ---------------- jumps ---------------- */
    __asm__ volatile("jal %0, 1f\n2:\tnop\n1:\tlla %1, 2b" : "=r"(r), "=r"(r2));
    check("jal", __LINE__, r, r2);
    __asm__ volatile("lla t1, 1f\n\tjalr %0, 1(t1)\n2:\tnop\n1:\tlla %1, 2b" : "=r"(r), "=r"(r2)::"t1");
    check("jalr", __LINE__, r, r2); /* bit 0 of the target is cleared */

    /* ---------------- CSRs ---------------- */
    csr_write(mscratch, 0x12345678u);
    __asm__ volatile("csrrs %0, mscratch, %1" : "=r"(r) : "r"(0xF0u));
    check("csrrs", __LINE__, r, 0x12345678u);
    check("csrrs", __LINE__, csr_read(mscratch), 0x123456F8u);
    __asm__ volatile("csrrc %0, mscratch, %1" : "=r"(r) : "r"(0xFFFF0000u));
    check("csrrc", __LINE__, csr_read(mscratch), 0x000056F8u);
    __asm__ volatile("csrrwi %0, mscratch, 21" : "=r"(r));
    check("csrrwi", __LINE__, r, 0x000056F8u);
    __asm__ volatile("csrrsi %0, mscratch, 10" : "=r"(r));
    check("csrrsi", __LINE__, csr_read(mscratch), 31);
    __asm__ volatile("csrrci %0, mscratch, 3" : "=r"(r));
    check("csrrci", __LINE__, csr_read(mscratch), 28);
    check("misa", __LINE__, csr_read(misa), 0x40100100u);
    check("mhartid", __LINE__, csr_read(mhartid), 0);
    r = csr_read(cycle);
    r2 = csr_read(cycle);
    check("cycle", __LINE__, r2 > r, 1);
    check("loadbase", __LINE__, psp_load_base() & 0xFFFu, 0);
    __asm__ volatile(".insn r 0x0b, 0, 0, %0, x0, x0" : "=r"(r)); /* psp.id */
    check("psp.id", __LINE__, r, 0x50535031u);

    /* ---------------- traps (M-mode) ---------------- */
    psp_set_trap_handler(handler);
    TRAPS(".word 0xffffffff", MCAUSE_ILLEGAL_INSN, 0xFFFFFFFFu);
    TRAPS("ebreak", MCAUSE_BREAKPOINT, 0xDEADu);
    TRAPS("ecall", MCAUSE_ECALL_M, 0);
    TRAPS(".insn r 0x0b, 1, 5, x0, x0, x0", MCAUSE_ILLEGAL_INSN, 0x0A00100Bu);
    TRAPS(".insn r 0x2b, 0, 0, x0, x0, x0", MCAUSE_ILLEGAL_INSN, 0x0000002Bu);
    TRAPS("csrr t0, 0x7c0", MCAUSE_ILLEGAL_INSN, 0x7C0022F3u);
    TRAPS("csrw cycle, zero", MCAUSE_ILLEGAL_INSN, 0xC0001073u);
    TRAPS(".word 0x02000033", MCAUSE_ILLEGAL_INSN, 0x02000033u); /* mul: no M extension */
    TRAPS(".word 0x02001013", MCAUSE_ILLEGAL_INSN, 0x02001013u); /* slli with shamt[5] = 1 */
    TRAPS("lla t0, mem\n\tlw t1, 1(t0)", 4, (uint32_t)(uintptr_t)(m + 1));
    TRAPS("lla t0, mem\n\tsw zero, 2(t0)", 6, (uint32_t)(uintptr_t)(m + 2));
    TRAPS("lw t1, 0(zero)", 5, 0);
    TRAPS("sb zero, 16(zero)", 7, 16);
    TRAPS("lla t1, 1f\n\taddi t1, t1, 2\n\tjalr x0, 0(t1)\n1:", 0, 0xDEADu); /* misaligned target */
    TRAPS("lla t1, 1f\n\tlla t2, t_recover\n\tsw t1, 0(t2)\n\tli t2, 0x10000000\n\tjalr x0, 0(t2)\n1:", 1,
          0x10000000u); /* fetch outside RAM (t0 would be a "link" register for the shadow stack) */

    /* ---------------- user mode ---------------- */
    static uint32_t ustack[256] __attribute__((aligned(16)));
    psp_enter_user(user_part, ustack + 256);
}
