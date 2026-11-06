"""A small benchmark suite for measuring the cost of Spectre defenses.

Each benchmark is a user program (generated from a seed) that stores a checksum
at its `result` label; the expected value is computed in Python, so a defense
that breaks program semantics is caught.  Programs avoid the registers gp and
tp, which the Part 4 compiler passes may use, and mark their entry with .entry.

    sum      bounds-checked array sum (well-predicted loop branch)
    bsearch  binary search: data-dependent, hard-to-predict branches
    list     linked-list walk (pointer chasing) with a slow data-dependent filter
    hist     byte histogram: table lookups indexed by loaded data
    filter   if (a[i]) sum += table[idx[i]]: lookups behind a data-dependent branch
    sort     insertion sort: compares on loaded values, stores
    syscall  calls the SYS_V1 and SYS_FR system calls with valid arguments
"""

import random

from .isa import M64


def _quads(vals, per=8):
    return "\n".join("    .quad " + ", ".join(str(v) for v in vals[k:k + per])
                     for k in range(0, len(vals), per))


def _bytes(vals, per=16):
    return "\n".join("    .byte " + ", ".join(str(v) for v in vals[k:k + per])
                     for k in range(0, len(vals), per))


def bench_sum(rng):
    n = 768
    vals = [rng.randrange(1 << 32) for _ in range(n)]
    src = f"""
.data
.align 64
n:      .quad {n}
result: .quad 0
.align 64
arr:
{_quads(vals)}
.text
_start:
.entry
    la      s0, arr
    la      t0, n
    ld      s1, 0(t0)           # n
    li      s2, 0               # i
    li      s3, 0               # sum
loop:
    bgeu    s2, s1, done        # bounds check / loop condition
    slli    t1, s2, 3
    add     t1, s0, t1
    ld      t2, 0(t1)
    add     s3, s3, t2
    addi    s2, s2, 1
    j       loop
done:
    la      t0, result
    sd      s3, 0(t0)
    halt
"""
    return src, sum(vals) & M64


def bench_bsearch(rng):
    n, nq = 512, 48
    arr = sorted(rng.sample(range(1, 1 << 40), n))
    keys = [rng.choice(arr) if rng.random() < 0.5 else rng.randrange(1, 1 << 40) for _ in range(nq)]
    found = sum(1 for k in keys if k in set(arr))
    src = f"""
.data
.align 64
result: .quad 0
.align 64
arr:
{_quads(arr)}
keys:
{_quads(keys)}
.text
_start:
.entry
    la      s0, arr
    la      s1, keys
    li      s2, {nq}            # queries left
    li      s3, 0               # found
    li      s5, {n}
query:
    beqz    s2, done
    ld      s4, 0(s1)           # key
    li      t0, 0               # lo
    mv      t1, s5              # hi
search:
    bgeu    t0, t1, check
    add     t2, t0, t1
    srli    t2, t2, 1           # mid
    slli    t3, t2, 3
    add     t3, s0, t3
    ld      t4, 0(t3)
    bgeu    t4, s4, go_left     # arr[mid] >= key
    addi    t0, t2, 1
    j       search
go_left:
    mv      t1, t2
    j       search
check:
    bgeu    t0, s5, next
    slli    t3, t0, 3
    add     t3, s0, t3
    ld      t4, 0(t3)
    bne     t4, s4, next
    addi    s3, s3, 1
next:
    addi    s1, s1, 8
    addi    s2, s2, -1
    j       query
done:
    la      t0, result
    sd      s3, 0(t0)
    halt
"""
    return src, found


def bench_list(rng):
    n, passes = 128, 8
    order = list(range(n))
    rng.shuffle(order)
    vals = [rng.randrange(1 << 20) for _ in range(n)]
    nxt = {order[k]: order[k + 1] for k in range(n - 1)}
    body = []
    for k in range(n):
        link = f"node{nxt[k]}" if k in nxt else "0"
        body.append(f"node{k}: .quad {link}, {vals[k]}")
    expected = (passes * sum(v for v in vals if v % 3 == 0)) & M64
    src = f"""
.data
.align 64
result: .quad 0
head:   .quad node{order[0]}
.align 64
{chr(10).join(body)}
.text
_start:
.entry
    li      s2, {passes}        # passes over the list
    li      s3, 0               # sum of values divisible by 3
    li      s6, 3
next_pass:
    beqz    s2, done
    la      t0, head
    ld      t0, 0(t0)
walk:
    beqz    t0, endpass
    ld      t1, 8(t0)           # node->val
    ld      t0, 0(t0)           # node = node->next   (pointer chase)
    remu    t2, t1, s6
    bnez    t2, walk            # slow, data-dependent filter
    add     s3, s3, t1
    j       walk
endpass:
    addi    s2, s2, -1
    j       next_pass
done:
    la      t0, result
    sd      s3, 0(t0)
    halt
"""
    return src, expected


def bench_hist(rng):
    n = 768
    data = [min(255, int(rng.expovariate(1 / 40))) for _ in range(n)]
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    expected = sum(counts[b] * (b + 1) for b in range(256)) & M64
    src = f"""
.data
.align 64
result: .quad 0
.align 64
counts: .zero 2048
data:
{_bytes(data)}
.text
_start:
.entry
    la      s0, data
    la      s1, counts
    li      s2, {n}
    li      s3, 0
loop:
    bgeu    s3, s2, sum
    add     t0, s0, s3
    lb      t1, 0(t0)           # byte
    slli    t1, t1, 3
    add     t1, s1, t1
    ld      t2, 0(t1)           # counts[byte]  (address depends on loaded data)
    addi    t2, t2, 1
    sd      t2, 0(t1)
    addi    s3, s3, 1
    j       loop
sum:
    li      s3, 0
    li      s4, 0
    li      s5, 256
sloop:
    bgeu    s3, s5, done
    slli    t1, s3, 3
    add     t1, s1, t1
    ld      t2, 0(t1)
    addi    t3, s3, 1
    mul     t2, t2, t3
    add     s4, s4, t2
    addi    s3, s3, 1
    j       sloop
done:
    la      t0, result
    sd      s4, 0(t0)
    halt
"""
    return src, expected


def bench_sort(rng):
    n = 48
    vals = [rng.randrange(1 << 32) for _ in range(n)]
    srt = sorted(vals)
    expected = sum(v * (k + 1) for k, v in enumerate(srt)) & M64
    src = f"""
.data
.align 64
result: .quad 0
.align 64
arr:
{_quads(vals)}
.text
_start:
.entry
    la      s0, arr
    li      s1, {n}
    li      s2, 1               # i
outer:
    bgeu    s2, s1, csum
    slli    t0, s2, 3
    add     t0, s0, t0
    ld      t1, 0(t0)           # key = arr[i]
    addi    t2, s2, -1          # j = i - 1
inner:
    blt     t2, zero, place
    slli    t3, t2, 3
    add     t3, s0, t3
    ld      t4, 0(t3)           # arr[j]
    bgeu    t1, t4, place       # key >= arr[j]: stop
    sd      t4, 8(t3)           # arr[j+1] = arr[j]
    addi    t2, t2, -1
    j       inner
place:
    addi    t3, t2, 1
    slli    t3, t3, 3
    add     t3, s0, t3
    sd      t1, 0(t3)
    addi    s2, s2, 1
    j       outer
csum:
    li      s2, 0
    li      s3, 0
cloop:
    bgeu    s2, s1, done
    slli    t0, s2, 3
    add     t0, s0, t0
    ld      t1, 0(t0)
    addi    t2, s2, 1
    mul     t1, t1, t2
    add     s3, s3, t1
    addi    s2, s2, 1
    j       cloop
done:
    la      t0, result
    sd      s3, 0(t0)
    halt
"""
    return src, expected


def bench_filter(rng):
    n, tsize = 512, 512
    a = [0 if rng.random() < 0.3 else rng.randrange(1, 1 << 16) for _ in range(n)]
    idx = [rng.randrange(tsize) for _ in range(n)]
    table = [rng.randrange(1 << 32) for _ in range(tsize)]
    expected = sum(table[idx[k]] for k in range(n) if a[k]) & M64
    src = f"""
.data
.align 64
result: .quad 0
.align 64
a:
{_quads(a)}
idx:
{_quads(idx)}
table:
{_quads(table)}
.text
_start:
.entry
    la      s0, a
    la      s3, idx
    la      s4, table
    li      s1, {n}
    li      s2, 0               # i
    li      s5, 0               # sum
loop:
    bgeu    s2, s1, done
    slli    t0, s2, 3
    add     t1, s0, t0
    ld      t2, 0(t1)           # a[i]
    beqz    t2, skip            # data-dependent filter
    add     t3, s3, t0
    ld      t4, 0(t3)           # idx[i]
    slli    t4, t4, 3
    add     t4, s4, t4
    ld      t5, 0(t4)           # table[idx[i]]  (address depends on loaded data)
    add     s5, s5, t5
skip:
    addi    s2, s2, 1
    j       loop
done:
    la      t0, result
    sd      s5, 0(t0)
    halt
"""
    return src, expected


def bench_syscall(rng):
    n = 48
    src = f"""
.data
.align 64
result: .quad 0
.text
_start:
.entry
    li      s2, {n}
    li      s3, 0               # sum of return values (all 0 when correct)
    li      s4, 0               # i
loop:
    beqz    s2, done
    andi    a0, s4, 15          # valid SYS_V1 index
    li      a7, SYS_V1
    syscall
    add     s3, s3, a0
    andi    a0, s4, 7           # valid SYS_FR index
    li      a7, SYS_FR
    syscall
    add     s3, s3, a0
    addi    s4, s4, 1
    addi    s2, s2, -1
    j       loop
done:
    add     s3, s3, s4
    la      t0, result
    sd      s3, 0(t0)
    halt
"""
    return src, n


BENCHMARKS = [("sum", bench_sum), ("bsearch", bench_bsearch), ("list", bench_list),
              ("hist", bench_hist), ("filter", bench_filter), ("sort", bench_sort),
              ("syscall", bench_syscall)]


def run_suite(make_machine, user_pass=None, seed=0):
    """Run every benchmark on a fresh machine from `make_machine()`.

    `user_pass` (callable str -> str) is applied to each benchmark's source, the
    way a compiler-based defense would recompile user code too.  Returns
    {name: (cycles, correct)}."""
    out = {}
    for k, (name, gen) in enumerate(BENCHMARKS):
        src, expected = gen(random.Random(seed * 101 + k))
        if user_pass is not None:
            src = user_pass(src)
        m = make_machine()
        prog = m.load(src, name=f"bench_{name}.s")
        res = m.run(prog, max_instrs=2_000_000)
        out[name] = (res.cycles, m.read_u64(prog["result"]) == expected)
    return out
