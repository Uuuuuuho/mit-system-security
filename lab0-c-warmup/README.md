# lab0-c-warmup: C crash course, "fix the logbook"

**Lecture:** L01 (introduction: why memory safety and undefined behaviour underlie most of the attacks in this course).
**Type:** native, any architecture (x86_64 or aarch64 Linux, Docker on Apple silicon is fine). No timing, no simulator needed.
**Estimated time:** 3-5 hours (Exercise 0-1: 2-4 h, Discussions: 1 h, Challenge: open-ended).

Later labs ask you to write C that touches memory in very deliberate ways: cache lines, page offsets,
pointer chasing, flexible buffers. This lab gets you comfortable with the tools that tell you when C
code does something you did not mean: AddressSanitizer, UndefinedBehaviorSanitizer, valgrind and gdb.
You get a small library that compiles without warnings and looks reasonable, and you have to make it
correct.

## Learning goals

- Read a precise C API specification and check an implementation against it.
- Recognise the classic bug classes: off-by-one on the NUL terminator, heap out-of-bounds writes,
  use-after-free, double free, leaks, uninitialized reads, mishandled `realloc` failure, integer
  overflow and underflow in size computations, signed-overflow undefined behaviour.
- Know which tool finds which bug class, and why some bugs are invisible in a normal `-O2` build.
- Connect a toy bug to a real one: CVE-2022-0185, a Linux kernel heap overflow caused by an unsigned
  length check that wraps around.

## Background

**The library.** `src/logbook.h` specifies a tiny append-only journal of tagged text entries. Read
the header first: it is the specification, and the implementation must follow it exactly. Two
things in it matter for this lab:

- *Flexible array members.* Both `struct logbook` (an array of entry pointers) and `struct lb_entry`
  (the text) end in a C99 flexible array member: `lb_entry *slots[];` and `char text[];`. The
  struct and its variable-length tail are one `malloc` block, so the size computation
  `sizeof(struct) + n * sizeof(element)` is written by hand. That is where off-by-ones and
  integer overflows live.
- *Blocks that move.* Growing the logbook `realloc`s the whole block, and `realloc` may return a
  different address. Any pointer to the old block is then dangling. This is why the growing
  functions take `logbook **lbp`.

**Undefined behaviour (UB).** In C, signed integer overflow, out-of-bounds access and using freed
memory are not "errors that crash". They are UB: the standard places no requirements on the
program, and optimizing compilers assume UB never happens. A program with UB often appears to work
at `-O2` today and breaks with the next compiler, flag or architecture.

**Unsigned wrap-around in length checks.** `size_t` arithmetic is modulo 2^64. A check like
`if (len > LIMIT - used - 2) reject;` is fine until `used + 2 > LIMIT`; then the right-hand side
wraps to a huge number and every `len` passes. CVE-2022-0185 (Linux `fs_context`,
`legacy_parse_param()`) had exactly this shape: `if (len > PAGE_SIZE - 2 - size)`. Once `size`
reached `PAGE_SIZE - 1`, the check passed for any length, and unprivileged users (via user namespaces)
got a heap overflow that was turned into a container escape. The fix was to rewrite the check as
an addition that cannot wrap: `if (size + len + 2 > PAGE_SIZE)`.

**Tools.**

| tool | how | finds | misses |
|---|---|---|---|
| AddressSanitizer (ASan) | `-fsanitize=address`: compiler instruments every load and store, allocator adds red zones and a quarantine of freed blocks | heap/stack/global out-of-bounds, use-after-free, double free, (LeakSanitizer) leaks | uninitialized reads, most UB |
| UBSan | `-fsanitize=undefined`: run-time checks inserted at UB-prone operations | signed overflow, misaligned or null pointer use, invalid shifts, too-small allocations for a type | memory errors in general |
| valgrind memcheck | runs the unmodified binary on a synthetic CPU that tracks which bytes are allocated and which bits are defined | invalid reads and writes, use of uninitialized values, leaks, bad frees | stack and global overflows, UB that has a defined machine meaning |
| allocation shim (this lab) | `tests/alloc_shim.c`, linked with `-Wl,--wrap=malloc,...` | leaks and double frees in plain builds, deterministic garbage in fresh memory, injected allocation failures | everything else |

## Files

```
src/logbook.h        the specification (do not change)
src/logbook.c        the implementation you fix                <- you work here
tests/tester.c       numbered behavioural checks (./tester CHECK_n)
tests/alloc_shim.c   allocation tracker used by the tester (leaks, double frees, fault injection)
tests/extra_tests.c  your own tests (Challenge 0-4)
check.py             grader: every check x {plain, asan, valgrind}
solutions/           reference implementation. SPOILERS: do not open it before you are done.
report.md            template for your written answers
```

## Build, run, check

```sh
make                  # builds ./tester (-O2) and ./tester-asan (ASan + UBSan)
./tester              # all checks, each in its own child process, with a summary
./tester --list
./tester CHECK_3      # one check in this process: ideal under gdb or valgrind
./tester-asan CHECK_3
valgrind --leak-check=full --track-origins=yes ./tester CHECK_7    # add SHIM_NOFILL=1, see below
make check            # the grader (~10 s)
make check-solution   # grades the reference solution (sanity check of your setup)
make clean
```

From the repository root on a Mac: `./scripts/lab-make.sh -C lab0-c-warmup check`.

`make check` runs every check in three modes and a check passes only if it passes in all of them:

| mode | binary | what it adds |
|---|---|---|
| plain | `./tester` | normal `-O2` build. The shim fails the check on leaks or double frees and fills fresh `malloc` memory with `0xA5` garbage |
| asan | `./tester-asan` | aborts at the first memory error or UB (`-fno-sanitize-recover=all`) |
| valgrind | `valgrind ./tester` | memcheck with `SHIM_NOFILL=1`, so valgrind itself sees uninitialized bytes |

The untouched starter passes only CHECK_1 (a sanity check). A correct library passes all 11.

## Part 1: fix the library

**Exercise 0-1.** `src/logbook.c` contains **ten planted bugs**. Fix all of them without changing
`logbook.h` or the tests, so that `make check` reports `11/11`. Keep each fix minimal and
local, and write down each one as you go (you will need the list for Discussion 0-2). Some functions
are correct; every function is marked `audit me` so the markers give nothing away.

Suggested workflow: run `make check` and pick one failing check. Read what that check does in
`tests/tester.c`. Run it in the mode that fails with the clearest message, find the root cause
(not just the line that crashes), fix it, and re-run. The checks are designed so that each one
targets one bug, but some bugs are visible in only one mode.

## Part 2: what went wrong, and what would have stopped it

**Discussion 0-2 (bug report).** For each of the ten bugs give: the function, the bug class, a
one-sentence root cause, which mode(s) of `make check` detected it and why the others did not,
and your fix. Then answer:
(a) Which bugs passed silently in the plain `-O2` build without the shim? Why is that dangerous?
(b) Which of your bugs is the same bug pattern as CVE-2022-0185? Show the arithmetic that makes the check
pass when it should fail, with concrete values.
(c) The signed-overflow bug gives the right answer in the plain build. Explain why that is not
evidence that it is harmless. What could a future compiler legally do?

**Discussion 0-3 (defenses).** For each bug, say whether each of the following would have *prevented*
it, *detected it at run time* (and at what cost), or *not helped*: `-D_FORTIFY_SOURCE=3`,
`-fwrapv` / `-ftrapv`, glibc's built-in heap consistency checks, a hardened allocator
(e.g. hardened_malloc or scudo), ARM MTE (memory tagging), CHERI capabilities, and writing the library in
Rust. One table plus a paragraph is enough. Which hardware mechanism covers the most rows, and what
does it cost in area, performance or compatibility? (This connects to L11 and the optional psp-lite shadow-stack exercise.)

## Challenge (optional)

**Challenge 0-4 (more tests).** Put new tests in `tests/extra_tests.c` and run `make extra` (built with
ASan + UBSan). On the unfixed library the example test already fails with a leak report. Ideas: edge cases
the tester misses, or a randomized differential test that applies thousands of random operations to
the logbook and to a trivial reference model and compares them after every step. Re-introduce
each bug one at a time: does your test suite catch all ten?

## Hints

<details><summary>General</summary>

- ASan's report has three parts: the bad access (with stack), the block it hit ("located N bytes
  after a M-byte region"), and where that block was allocated and possibly **freed**. If you see
  "freed by thread T0", the bug is a use-after-free even if the headline says overflow.
- `valgrind --track-origins=yes` tells you where an uninitialized value came from.
- `gdb ./tester-asan` then `run CHECK_n`; ASan stops at the bug, `bt` and `frame N` work.
- Before you call a size computation correct, try extreme inputs: 0, 1, the limit, SIZE_MAX.
- `char` is signed on x86_64 and unsigned on aarch64. This lab does not depend on that, but a
  hash function that uses plain `char` would give different results on your Mac and the cloud box.
</details>

<details><summary>If you are stuck on a particular check</summary>

- CHECK_2: what does `realloc` do to pointers into the old block? Which local variables still point there?
- CHECK_5: after a `free`, what should happen to the pointer variable?
- CHECK_8: compute `LB_LABELS_MAX - 2 - labels_len` by hand for every step of the test.
- CHECK_10: compute `sizeof(logbook) + cap * sizeof(lb_entry *)` for `cap = SIZE_MAX / 4`.
- CHECK_11: what does `realloc` return on failure, and what happens to the old block?
</details>

## Going further

- Compile with `clang -fsanitize=memory` (MSan) and compare with valgrind on the uninitialized read.
- Build with `-fanalyzer` (GCC) or `scan-build` (clang static analyzer). Which bugs are found statically?
- Read the CVE-2022-0185 write-ups and the one-line kernel fix in `fs/fs_context.c` (January 2022).
- Look at how the Linux kernel avoids this class of bug with `check_add_overflow()`, `struct_size()` and
  `size_add()` from `<linux/overflow.h>`. Rewrite `lb_reserve()` in that style.

## References

- K. Serebryany et al., "AddressSanitizer: A Fast Address Sanity Checker", USENIX ATC 2012.
- N. Nethercote and J. Seward, "Valgrind: A Framework for Heavyweight Dynamic Binary Instrumentation", PLDI 2007.
- X. Wang et al., "Towards Optimization-Safe Systems: Analyzing the Impact of Undefined Behavior", SOSP 2013.
- CVE-2022-0185, Linux kernel `legacy_parse_param()` integer underflow (Jan 2022).
- ISO/IEC 9899:2011 (C11), section 6.7.2.1 (flexible array members) and 6.5 (signed overflow is undefined).
- J. Woodruff et al., "The CHERI capability model", ISCA 2014; Arm, "Memory Tagging Extension" whitepaper, 2019.
