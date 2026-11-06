# fib.s -- Fibonacci with the loop counter kept in memory (only 3 usable registers!)
        addi  x1, x0, 0          # a = F(0)
        addi  x2, x0, 1          # b = F(1)
        addi  x3, x0, 10
        sw    x3, 0(x0)          # n = 10
loop:   lw    x3, 0(x0)
        beq   x3, x0, done
        addi  x3, x3, -1
        sw    x3, 0(x0)
        add   x3, x1, x2         # t = a + b
        add   x1, x2, x0         # a = b
        add   x2, x3, x0         # b = t
        j     loop
done:   halt
#! x1=55 x2=89 x3=0 mem0=0
