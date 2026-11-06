/* solutions/shadow_stack.c - REFERENCE shadow stack for psp-lite (SPOILER for the optional shadow-stack exercise).
 *
 * Policy:
 *   - every call pushes its return address onto a stack held inside the core;
 *   - every return pops one entry and must jump exactly there;
 *   - overflow (deeper than SS_DEPTH) and underflow are faults as well, so an
 *     attacker cannot "drain" the stack with unbalanced returns.
 * This is the same contract as Intel CET's shadow stack or RISC-V Zicfiss
 * (sspush/sspopchk), minus the special memory pages: our stack is not
 * addressable by the program at all.
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
    ss->pushes++;
    if (ss->top >= SS_DEPTH) {
        ss->fault_expected = 0;
        ss->fault_actual = ret_addr;
        return SS_FAULT_OVERFLOW;
    }
    ss->entry[ss->top++] = ret_addr;
    return SS_OK;
}

int ss_on_return(psp_shadow_stack *ss, uint32_t pc, uint32_t target) {
    (void)pc;
    ss->pops++;
    if (ss->top == 0) {
        ss->fault_expected = 0;
        ss->fault_actual = target;
        return SS_FAULT_UNDERFLOW;
    }
    uint32_t expected = ss->entry[--ss->top];
    if (expected != target) {
        ss->fault_expected = expected;
        ss->fault_actual = target;
        return SS_FAULT_MISMATCH;
    }
    return SS_OK;
}
