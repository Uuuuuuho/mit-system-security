# Lab 3 report: transient execution in `specsim`

Name:
Seed(s) used for the numbers below (`SEED=...`):

## Discussion 1-1: hit/miss latencies and the threshold

*Distributions (min / median / max, or a small histogram) for L1 hits, L2 hits and DRAM; your threshold
and why. Why does the slot that hits during the reload usually show L2 rather than L1 latency (L1/L2 set
of `PROBE_BASE + b * 4096`)?*

## Discussion 1-2: why a correct system call leaks

*Why `SYS_FR` leaks without any speculation. A secret-independent rewrite in toy assembly, its cost, and
which Part 4 defenses (if any) you expect to stop this leak.*

## Discussion 2-1: timeline of one attack call

| event | cycle |
|---|---|
| bound load issues / completes | |
| branch resolves | |
| secret load issues / completes | |
| transmitting load issues | |
| number of transient instructions | |

*Window size and comments.*

## Discussion 2-2: first attempt, training, gshare

*Why the first attempt per byte fails; what if the secret were never cached; how many training calls and
why; behaviour with `Machine(ghist_bits=8)`.*

## Discussion 2-3: limits on speculation

*What bounds the wrong path besides the bound's latency; maximum transient instruction count estimate.*

## Discussion 3-1: window versus critical path

| bound location | window (cycles) |
|---|---|
| L1 | |
| L2 | |
| DRAM | |

*Gadget critical path from branch dispatch to transmit issue, and why the Part 2 attack fails on
`SYS_V1_HARD`.*

## Discussion 3-2: eviction set design

*Set-index bits, congruence, why L1 collides too, number of lines under LRU, role of inclusion;
`l2_repl="random"`; hashed/sliced indexing.*

## Discussion 4-1: leakage and overhead

*Paste the defense table printed by `make check` and explain each defense's cost on at least two
benchmarks.*

| defense | leaked | arch F+R | geomean | sum | bsearch | list | hist | filter | sort | syscall |
|---|---|---|---|---|---|---|---|---|---|---|
| baseline | | | | | | | | | | |
| fence | | | | | | | | | | |
| slh | | | | | | | | | | |
| invisispec | | | | | | | | | | |
| stt | | | | | | | | | | |

## Discussion 4-2: hardware-software contracts

*What each defense guarantees about speculative leakage relative to sequential execution, why Part 1 still
leaks, and what software must guarantee itself (which contract `SYS_FR` needs).*

## Discussion 4-3: what the defenses miss

*One attack that one of your defenses does not stop in principle, and an experiment in `specsim` that would
demonstrate it.*
