# trap.s -- ECALL from M and U, illegal instructions, CSR access from U, MRET.
# The handler logs each mcause value to memory and resumes after the trap.
        la    x1, handler
        csrw  mtvec, x1
        addi  x3, x0, 0          # x3 = log pointer
        ecall                    # from M: mcause = 11, returns here (MPP = M)
        la    x1, user
        csrw  mepc, x1
        csrw  mstatus, x0        # MPP = U
        mret                     # -> user code in U-mode
handler:
        csrrw x1, mepc, x0       # x1 = address of the trapping instruction
        addi  x1, x1, 4
        csrw  mepc, x1           # resume after it
        csrrw x2, mcause, x0     # x2 = cause (and clear mcause)
        addi  x3, x3, 4
        sw    x2, 0(x3)          # log it: words 1, 2, 3, 0
        mret
user:   ecall                    # from U: mcause = 8
        .word 0x0000000b         # custom-0 opcode: reserved -> illegal, mcause = 2
        csrrw x1, mtvec, x0      # M-mode CSR from U -> illegal, mcause = 2
        halt                     # (still in U-mode)
#! x1=0x50 x2=2 x3=16 priv=U mtvec=0x28 mepc=0x50 mcause=0 mpp=U mem0=2 mem1=11 mem2=8 mem3=2
