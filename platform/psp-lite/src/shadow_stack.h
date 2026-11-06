/* shadow_stack.h - the "hardware shadow stack" hook of psp-lite (optional exercise).
 *
 * When psp-lite runs with --shadow-stack, the core classifies every JAL/JALR
 * using the RISC-V return-address-stack hints (x1/ra and x5/t0 are "link"
 * registers) and calls the two hooks below:
 *
 *   JAL  rd=link                          -> ss_on_call
 *   JALR rd=link, rs1 not link            -> ss_on_call
 *   JALR rd not link, rs1=link            -> ss_on_return      (a plain "ret")
 *   JALR rd=link, rs1=link, rd != rs1     -> ss_on_return, then ss_on_call
 *   JALR rd=link, rs1=link, rd == rs1     -> ss_on_call
 *
 * A hook returns SS_OK to let the instruction retire, or any other value to make
 * the core raise a software-check exception instead (mcause = 18, mtval = 3,
 * mepc = the JAL/JALR; the instruction has no effect).  If no trap handler is
 * installed (mtvec == 0) psp-lite stops with exit status 64 + 18 = 82 and prints
 * the fault, including ss->fault_expected / ss->fault_actual.
 *
 * The shadow stack lives inside the simulated "hardware" (this struct), so the
 * program running on psp-lite cannot read or write it with loads and stores.
 *
 * The implementation file is chosen at build time:
 *     make SHADOW_STACK_SRC=/path/to/your/shadow_stack.c
 * (default: src/shadow_stack.c, an empty stub that you complete in the optional exercise).
 */
#ifndef PSP_SHADOW_STACK_H
#define PSP_SHADOW_STACK_H

#include <stdint.h>

#define SS_DEPTH 1024

enum { SS_OK = 0, SS_FAULT_MISMATCH = 1, SS_FAULT_UNDERFLOW = 2, SS_FAULT_OVERFLOW = 3 };

typedef struct psp_shadow_stack {
    uint32_t entry[SS_DEPTH]; /* saved return addresses           */
    int top;                  /* number of valid entries           */
    uint64_t pushes, pops;    /* statistics (printed by --stats)   */
    /* diagnostics: fill these in before returning a fault code */
    uint32_t fault_expected; /* return address the stack expected */
    uint32_t fault_actual;   /* target the program tried to use    */
} psp_shadow_stack;

/* Called once before the program starts. */
void ss_reset(psp_shadow_stack *ss);

/* A call at `pc` jumps to `target`; the matching return should come back to `ret_addr` (= pc + 4). */
int ss_on_call(psp_shadow_stack *ss, uint32_t pc, uint32_t target, uint32_t ret_addr);

/* A return at `pc` is about to jump to `target`. */
int ss_on_return(psp_shadow_stack *ss, uint32_t pc, uint32_t target);

#endif
