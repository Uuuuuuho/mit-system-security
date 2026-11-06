/* trap_demo.c - privilege levels and traps on psp-lite.
 *
 * main() runs in M-mode, installs a trap handler written in C (clang's
 * interrupt("machine") attribute saves registers and returns with mret),
 * then drops to U-mode.  User code makes an ecall ("system call"), tries to
 * read an M-mode-only CSR, and executes an undefined custom-0 instruction;
 * every one of these lands in the handler, which skips the faulting
 * instruction by advancing mepc.
 */
#include "psp.h"

static volatile uint32_t ntraps, last_cause;

__attribute__((interrupt("machine"))) static void trap_handler(void) {
    uint32_t cause = csr_read(mcause), epc = csr_read(mepc);
    ntraps++;
    last_cause = cause;
    printf("  [M] trap: mcause=%u mepc=%p mtval=%08x\n", cause, (void *)epc, csr_read(mtval));
    csr_write(mepc, epc + 4); /* skip the instruction (all RV32I instructions are 4 bytes) */
}

static void user_main(void) {
    uint32_t v = 0;
    printf("[U] hello from user mode\n");
    __asm__ volatile("li a7, 64\n\tecall" ::: "a7");
    __asm__ volatile("csrr %0, 0xFC0" : "=r"(v)); /* flag CSR: M-mode only -> traps */
    __asm__ volatile(".insn r 0x0b, 3, 0x11, x0, x0, x0"); /* undefined custom-0 encoding */
    printf("[U] %u traps handled, last mcause=%u; v=%08x (unchanged)\n", ntraps, last_cause, v);
    psp_exit(0); /* the exit CSR is user-accessible */
}

static uint32_t user_stack[512] __attribute__((aligned(16)));

int main(void) {
    char flag[33];
    psp_read_flag(flag);
    printf("[M] the flag CSRs are readable here: %s\n", flag);
    psp_set_trap_handler(trap_handler);
    psp_enter_user(user_main, user_stack + 512);
}
