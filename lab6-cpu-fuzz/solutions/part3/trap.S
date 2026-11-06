/* trap.S - Part 3: machine-mode trap handler (REFERENCE SOLUTION - spoiler).
 *
 * Every trap (from U- or M-mode) lands here.  We record the cause, count the
 * trap, and resume at the instruction after the faulting one (mepc + 4: all
 * RV32I instructions are 4 bytes).  Only t0/t1 are used and they are saved on
 * the interrupted stack (the RISC-V ABI has no red zone, so the area below sp
 * is free).  This handler never returns to the faulting instruction, so it
 * cannot loop forever.
 */
    .text
    .globl trap_entry
    .type trap_entry, @function
    .balign 4
trap_entry:
    addi    sp, sp, -16
    sw      t0, 0(sp)
    sw      t1, 4(sp)

    csrr    t0, mcause
    lla     t1, last_cause
    sw      t0, 0(t1)

    lla     t1, trap_count
    lw      t0, 0(t1)
    addi    t0, t0, 1
    sw      t0, 0(t1)

    csrr    t0, mepc
    addi    t0, t0, 4
    csrw    mepc, t0

    lw      t0, 0(sp)
    lw      t1, 4(sp)
    addi    sp, sp, 16
    mret
    .size trap_entry, . - trap_entry
