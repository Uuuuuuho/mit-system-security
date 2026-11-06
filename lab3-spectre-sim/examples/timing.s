# Time one cache hit and one cache miss.
#   python3 -m specsim run examples/timing.s            (results in a1 / a2)
#   python3 -m specsim run examples/timing.s --trace    (per-instruction timing)
.data
.align 64
x:  .zero 64
.text
_start:
    la      s0, x
    lb      t0, 0(s0)           # bring x into the cache
    rdcycle t1                  # rdcycle waits for all older instructions
    lb      t0, 0(s0)           # hit
    rdcycle t2
    sub     a1, t2, t1          # a1 = hit latency (+1)
    clflush 0(s0)               # remove x from L1 and L2
    rdcycle t1
    lb      t0, 0(s0)           # miss: goes to DRAM
    rdcycle t2
    sub     a2, t2, t1          # a2 = miss latency (+1)
    halt
