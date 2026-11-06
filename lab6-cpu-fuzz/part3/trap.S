/* trap.S - Part 3: machine-mode trap handler (STARTER).
 *
 * Exercise 3-1: every trap (from U- or M-mode) lands in trap_entry.  It must
 *   - store mcause into the C variable last_cause,
 *   - increment the C variable trap_count,
 *   - resume at the instruction AFTER the faulting one (all RV32I instructions
 *     are 4 bytes), and return with mret,
 *   - preserve every register it uses (save them on the interrupted stack).
 * As shipped it just spins, so any trap hangs the program (the simulator stops
 * it at --max-instr).
 */
    .text
    .globl trap_entry
    .type trap_entry, @function
    .balign 4
trap_entry:
    /* TODO (Exercise 3-1) */
1:  j       1b
    .size trap_entry, . - trap_entry
