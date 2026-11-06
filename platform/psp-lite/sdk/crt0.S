/* crt0.S - psp-lite start-up code.
 *
 * Programs are linked as position-independent executables at 0x80000000
 * (link.ld).  psp-lite may load the image somewhere else (--aslr, --base), so
 * the first thing we do is relocate ourselves, exactly like a self-relocating
 * boot loader:
 *     slide = (where _start really is) - (where the linker put it)
 *     for each R_RISCV_RELATIVE entry:  *(r_offset + slide) = r_addend + slide
 * Then: set up the stack, clear .bss, leave mtvec = 0 (no trap handler: the
 * simulator reports traps itself), call main(), and pass its return value to
 * the exit CSR.
 */
    .section .text.init, "ax", @progbits
    .globl _start
    .type _start, @function
_start:
    lla     t0, _start                 /* run-time address (PC-relative)        */
    lui     t1, %hi(__psp_link_base)   /* link-time address (absolute symbol)   */
    addi    t1, t1, %lo(__psp_link_base)
    sub     s0, t0, t1                 /* s0 = slide                            */

    lla     t2, __rela_start
    lla     t3, __rela_end
1:  bgeu    t2, t3, 3f
    lw      t4, 4(t2)                  /* r_info                                */
    beqz    t4, 2f                     /* R_RISCV_NONE                          */
    li      t5, 3                      /* R_RISCV_RELATIVE                      */
    bne     t4, t5, .Lbad_reloc
    lw      t4, 0(t2)                  /* r_offset (link-time address)          */
    lw      t5, 8(t2)                  /* r_addend                              */
    add     t4, t4, s0
    add     t5, t5, s0
    sw      t5, 0(t4)
2:  addi    t2, t2, 12
    j       1b
3:
    lla     sp, __stack_top
    lla     t0, __bss_start
    lla     t1, __bss_end
4:  bgeu    t0, t1, 5f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       4b
5:
    csrw    mtvec, zero
    li      a0, 0
    li      a1, 0
    call    main
    csrw    0x802, a0                  /* exit(main())                          */
6:  j       6b

.Lbad_reloc:                           /* not a static PIE we understand        */
    ebreak
    j       .Lbad_reloc
    .size _start, . - _start

/* void psp_enter_user(void (*fn)(void), void *user_sp)  -- M -> U, never returns */
    .text
    .globl psp_enter_user
    .type psp_enter_user, @function
psp_enter_user:
    csrw    mepc, a0
    li      t0, 0x1800                 /* mstatus.MPP                           */
    csrc    mstatus, t0                /* MPP = U                               */
    beqz    a1, 1f
    mv      sp, a1
1:  mret
    .size psp_enter_user, . - psp_enter_user
