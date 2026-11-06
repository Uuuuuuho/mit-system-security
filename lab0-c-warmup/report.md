# lab0-c-warmup report

Name:
Date:
`make check` result (paste the score line):

## Exercise 0-1: summary of changes

(Optional: a short diff summary or a list of the lines you changed. The details go in Discussion 0-2.)

## Discussion 0-2: bug report

| # | function | bug class | root cause (one sentence) | detected by (plain / asan / valgrind) and why | fix |
|---|---|---|---|---|---|
| 1 | | | | | |
| 2 | | | | | |
| 3 | | | | | |
| 4 | | | | | |
| 5 | | | | | |
| 6 | | | | | |
| 7 | | | | | |
| 8 | | | | | |
| 9 | | | | | |
| 10 | | | | | |

(a) Bugs that are silent in a plain -O2 build, and why that is dangerous:

(b) The CVE-2022-0185 pattern, with concrete values:

(c) Why "the signed-overflow version gives the right answer" is not evidence of safety:

## Discussion 0-3: defenses

| bug | _FORTIFY_SOURCE=3 | -fwrapv / -ftrapv | glibc heap checks | hardened allocator | MTE | CHERI | Rust |
|---|---|---|---|---|---|---|---|
| 1 | | | | | | | |
| 2 | | | | | | | |
| 3 | | | | | | | |
| 4 | | | | | | | |
| 5 | | | | | | | |
| 6 | | | | | | | |
| 7 | | | | | | | |
| 8 | | | | | | | |
| 9 | | | | | | | |
| 10 | | | | | | | |

Which hardware mechanism covers the most rows, and at what cost:

## Challenge 0-4: extra tests (optional)

What you tested, and which re-introduced bugs your tests catch:
