# branch.s -- BEQ loops and JAL (with link) : sum 1..5
        addi  x1, x0, 5          # counter
        addi  x2, x0, 0          # sum
loop:   beq   x1, x0, done
        add   x2, x2, x1
        addi  x1, x1, -1
        j     loop               # jal x0, loop
done:   lui   x1, 0x10           # x1 = 0x00010000 (low 16 bits are all zero)
        beq   x1, x0, bad        # must NOT be taken: compares all 32 bits
        jal   x3, next           # x3 = address of 'bad' (pc + 4)
bad:    addi  x2, x2, 100
next:   halt
#! x1=0x10000 x2=15 x3=0x24 priv=M
