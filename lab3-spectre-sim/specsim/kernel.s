# =============================================================================
# specsim toy kernel
#
# Assembled at boot and run in kernel mode.  User code enters only through
# `syscall` (number in a7, argument in a0) and the kernel returns with `sysret`
# (result in a0).  The kernel clobbers t0-t6 and a0 (and gp/tp when a Part 4
# compiler pass is applied).  It has no stack.
#
# Every k_* data symbol is public, like a System.map without KASLR: user
# programs may use the addresses as constants, but loading from them in user
# mode faults.  The secrets are written into k_secret1..3 at boot.
# =============================================================================

.text
syscall_entry:
.entry
    li      t0, SYS_FR
    beq     a7, t0, sys_fr
    li      t0, SYS_V1
    beq     a7, t0, sys_v1
    li      t0, SYS_V1_HARD
    beq     a7, t0, sys_v1_hard
    li      t0, SYS_NOP
    beq     a7, t0, sys_nop
    li      a0, -1                  # unknown syscall number
    sysret

sys_nop:
    li      a0, 0
    sysret

# -----------------------------------------------------------------------------
# SYS_FR (Part 1) -- a "lookup service" that touches the shared slot
#     PROBE[secret1[i] * 4096]
# It is bounds-checked and architecturally correct, yet its memory access
# pattern depends on the secret.
#     a0 = i   ->   a0 = 0, or -1 if i >= len(secret1)
# -----------------------------------------------------------------------------
sys_fr:
    la      t0, k_fr_len
    ld      t1, 0(t0)
    bgeu    a0, t1, sys_fr_bad
    la      t2, k_secret1
    add     t2, t2, a0
    lb      t3, 0(t2)               # secret byte
    slli    t3, t3, 12              # * PROBE_STRIDE
    li      t4, PROBE_BASE
    add     t4, t4, t3
    lb      t5, 0(t4)               # touch the shared slot
    li      a0, 0
    sysret
sys_fr_bad:
    li      a0, -1
    sysret

# -----------------------------------------------------------------------------
# SYS_V1 (Part 2) -- the classic Spectre v1 gadget
#     if (x < v1_limit) touch PROBE[v1_array[x] * 4096]
# The kernel flushes v1_limit on every call, so the bounds check has to wait
# for DRAM.  v1_array holds 16 public bytes.
#     a0 = x   ->   a0 = 0
# -----------------------------------------------------------------------------
sys_v1:
    la      t0, k_v1_limit
    clflush 0(t0)
    ld      t1, 0(t0)               # bound (always a cache miss)
    bgeu    a0, t1, sys_v1_out      # bounds check
    la      t2, k_v1_array
    add     t2, t2, a0
    lb      t3, 0(t2)               # v1_array[x]
    slli    t3, t3, 12
    li      t4, PROBE_BASE
    add     t4, t4, t3
    lb      t5, 0(t4)               # PROBE[v1_array[x] * 4096]
sys_v1_out:
    li      a0, 0
    sysret

# -----------------------------------------------------------------------------
# SYS_V1_HARD (Part 3) -- the same gadget, but
#   * the bound is NOT flushed: it normally sits in L1 between calls, and
#   * the index goes through a slow dependent computation (mul/divu by table
#     geometry kept in kernel variables) before the array load.
#     a0 = x   ->   a0 = 0
# -----------------------------------------------------------------------------
sys_v1_hard:
    la      t0, k_v1h_limit
    ld      t1, 0(t0)               # bound (cached, unless someone evicted it)
    bgeu    a0, t1, sys_v1h_out
    la      t2, k_v1h_geom
    ld      t3, 0(t2)               # element size      (= 1)
    ld      t4, 8(t2)               # elements per slot (= 1)
    mul     t5, a0, t3              # offset = x * size / per_slot, computed twice
    divu    t5, t5, t4              # (a stand-in for any slow, data-dependent
    mul     t5, t5, t3              #  address computation)
    divu    t5, t5, t4
    la      t2, k_v1h_array
    add     t2, t2, t5
    lb      t3, 0(t2)
    slli    t3, t3, 12
    li      t4, PROBE_BASE
    add     t4, t4, t3
    lb      t5, 0(t4)
sys_v1h_out:
    li      a0, 0
    sysret

# =============================================================================
.data
    .zero   64                      # first line unused
k_fr_len:
    .quad   0                       # len(secret1), set at boot
    .align  64
k_v1_limit:
    .quad   16
    .align  64
k_v1_array:
    .zero   16                      # 16 public bytes, randomized at boot
    .align  64
k_v1h_limit:
    .quad   16
    .align  64
k_v1h_geom:
    .quad   1, 1
    .align  64
k_v1h_array:
    .zero   16                      # 16 public bytes, randomized at boot
    .align  2048
k_secret1:                          # Part 1 secret (FLAG{...}, <= 48 bytes)
    .zero   64
k_secret2:                          # Part 2 / Part 4 secret
    .zero   64
k_secret3:                          # Part 3 secret
    .zero   64
