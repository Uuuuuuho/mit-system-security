/* shadow_stack.c - STUDENT HOOK (optional exercise): hardware shadow stack for psp-lite.
 *
 * This file is compiled into psp-lite.  The core (cpu.c) calls these functions
 * for every call/return when the simulator runs with --shadow-stack; read
 * shadow_stack.h for the exact classification and the fault protocol.
 *
 * As shipped, the hooks do nothing: every return is allowed, so --shadow-stack
 * has no effect and a corrupted return address is only noticed later (if at all).
 * The exercise: make the hardware remember return addresses and refuse a return
 * that does not match (see examples/ss_demo.c and `make smoke`).
 *
 * Build psp-lite with your copy of this file:
 *     make -C platform/psp-lite BUILD=$PWD/build/psp SHADOW_STACK_SRC=$PWD/shadow_stack.c DEFECTS=0
 * A reference implementation is in platform/psp-lite/solutions/shadow_stack.c (spoiler).
 */
#include "shadow_stack.h"

void ss_reset(psp_shadow_stack *ss) {
    ss->top = 0;
    ss->pushes = ss->pops = 0;
    ss->fault_expected = ss->fault_actual = 0;
}

int ss_on_call(psp_shadow_stack *ss, uint32_t pc, uint32_t target, uint32_t ret_addr) {
    (void)pc;
    (void)target;
    (void)ret_addr;
    ss->pushes++;
    /* ===================== LAB5 TODO (Exercise 4-1) =====================
     * Remember ret_addr.  Decide what the hardware should do when the stack
     * is full (SS_DEPTH entries) and return SS_FAULT_OVERFLOW in that case.
     * ==================================================================== */
    return SS_OK;
}

int ss_on_return(psp_shadow_stack *ss, uint32_t pc, uint32_t target) {
    (void)pc;
    (void)target;
    ss->pops++;
    /* ===================== LAB5 TODO (Exercise 4-1) =====================
     * Pop the most recent return address and compare it with `target`.
     * On a mismatch set ss->fault_expected / ss->fault_actual and return
     * SS_FAULT_MISMATCH; on an empty stack return SS_FAULT_UNDERFLOW.
     * ==================================================================== */
    return SS_OK;
}
