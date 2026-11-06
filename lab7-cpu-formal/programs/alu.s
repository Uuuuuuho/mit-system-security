# alu.s -- LUI / ADDI / ADD / XOR / SRLI, writes to x0, register aliasing.
# Only uses the ALU instructions, so it is a useful co-simulation test in Part 1.
        lui   x1, 0x80000        # x1 = 0x80000000
        addi  x1, x1, -1         # x1 = 0x7fffffff
        addi  x2, x0, 1          # x2 = 1
        add   x3, x1, x2         # x3 = 0x80000000   (wraps, no overflow trap)
        xor   x3, x3, x1         # x3 = 0xffffffff
        srli  x2, x3, 28         # x2 = 0x0000000f   (logical: zeros shifted in)
        srli  x3, x3, 0          # x3 = 0xffffffff
        add   x3, x3, x2         # x3 = 0x0000000e   (carry out is dropped)
        addi  x0, x1, 5          # writes to x0 are ignored
        addi  x5, x0, 0x123      # x5 aliases x1 (only bits [1:0] are decoded)
        xor   x2, x2, x0         # x2 = 0xf
        lui   x4, 0xabcde        # x4 aliases x0 -> discarded
        halt
#! x0=0 x1=0x123 x2=0xf x3=0xe priv=M
