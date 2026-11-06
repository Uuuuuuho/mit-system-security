# mem.s -- LW / SW, signed offsets, and the 16-byte aliasing data memory
        li    x1, 0x11111111     # lui + addi
        sw    x1, 0(x0)          # mem0 = 0x11111111
        li    x2, 0x2222
        sw    x2, 4(x0)          # mem1 = 0x2222
        addi  x3, x0, 12
        sw    x2, -4(x3)         # address  8 -> word 2
        sw    x1, 16(x3)         # address 28 -> word 3 (bits [3:2] = 11)
        lw    x1, 20(x0)         # address 20 -> word 1 = 0x2222
        lw    x2, 3(x3)          # address 15 -> word 3 (bits [1:0] ignored)
        lw    x0, 0(x0)          # x0 stays 0
        addi  x3, x0, 7
        sw    x3, 0(x0)          # mem0 = 7
        lw    x3, 32(x0)         # address 32 aliases word 0
        halt
#! x0=0 x1=0x2222 x2=0x11111111 x3=7 mem0=7 mem1=0x2222 mem2=0x2222 mem3=0x11111111
