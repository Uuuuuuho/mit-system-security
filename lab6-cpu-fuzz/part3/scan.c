/* scan.c - Part 3: detecting undocumented custom-0 instructions
 * (STARTER).
 *
 *  1. main() (M-mode) installs trap_entry and drops to U-mode.
 *  2. user_main() writes each custom-0 encoding (R-type layout, funct7 x funct3,
 *     rd = rs1 = rs2 = x0) into an executable slot and calls it.  The trap
 *     handler counts traps; an encoding that does not trap and is not the
 *     documented psp.id (funct3 = funct7 = 0) is undocumented behaviour that a
 *     verification team must explain (it could be a debug leftover or a backdoor).
 *
 * Output (parsed by the grader):
 *   undocumented=0x<encoding>      one line per non-trapping undocumented encoding
 * or "undocumented=none" when every undocumented encoding traps.
 */
#include "psp.h"

#define CUSTOM0 0x0Bu

extern void trap_entry(void);
volatile uint32_t trap_count; /* updated by trap_entry */
volatile uint32_t last_cause;

/* executable slot: [instruction under test] [ret]; RAM has no execute protection */
static uint32_t slot[2] __attribute__((aligned(8)));
typedef void (*slot_fn)(uint32_t a0);

static void load_slot(uint32_t insn) {
    slot[0] = insn;
    slot[1] = 0x00008067u; /* jalr x0, 0(ra) */
    __asm__ volatile("fence.i" ::: "memory");
}

static int runs_without_trap(uint32_t a0) {
    uint32_t before = trap_count;
    ((slot_fn)(uintptr_t)slot)(a0);
    return trap_count == before;
}

static void user_main(void) {
    /* TODO (Exercise 3-2): for every funct7 (0..127) and funct3 (0..7) except the
     * documented psp.id (both 0), build the custom-0 R-type word
     *     (funct7 << 25) | (funct3 << 12) | CUSTOM0      (rd = rs1 = rs2 = x0)
     * load it into the slot, run it, and print "undocumented=0x%08x" for every
     * encoding that does NOT trap.  Print "undocumented=none" if all of them trap.
     * (runs_without_trap() only works once trap.S is done - Exercise 3-1.) */
    (void)load_slot;
    (void)runs_without_trap;
    printf("undocumented=none\n");
    psp_exit(0);
}

static uint32_t user_stack[1024] __attribute__((aligned(16)));

int main(void) {
    psp_set_trap_handler(trap_entry);
    printf("scan: starting in M-mode, dropping to U-mode\n");
    psp_enter_user(user_main, user_stack + 1024);
}
