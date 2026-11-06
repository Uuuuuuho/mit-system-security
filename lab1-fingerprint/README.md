# lab1-fingerprint: sweep-counting website fingerprinting, natively

**Lecture:** L02 (side channels by example: website fingerprinting, and how to tell *which*
microarchitectural effect an attack really exploits).
**Type:** native, any architecture: x86_64 or aarch64 Linux, including Docker on Apple silicon. No
simulator backend is needed: every measurement here is *aggregate* work per millisecond-scale window
read with `clock_gettime`, which works even with the 24 MHz counter of an Apple VM (that is the
point of Part 1).
**Estimated time:** Part 1 1-1.5 h, Part 2 3-4 h, Part 3 2-3 h.

In 2019 Shusterman et al. showed that JavaScript in one browser tab can tell which website another tab
is loading. The attacker sweeps a last-level-cache-sized buffer over and over and counts how many
sweeps complete in each window of P milliseconds. The trace of counts, fed to a machine-learning
classifier, identifies the site. The usual explanation is cache occupancy: the victim evicts the
attacker's lines, so sweeps get slower. In 2022 Cook et al. removed every memory access from the
attacker loop and the attack *still worked*. In this lab you rebuild both experiments natively,
against synthetic "websites" in `sites/`, and work out for yourself what the signal is on your machine.

## Learning goals

- Quantify how timer resolution limits what an attacker can measure, and how aggregating many
  events gets around a coarse timer.
- Build a complete side-channel pipeline: collector, victim workloads, synchronized trace collection,
  train/test methodology, classifier, report.
- Do a root-cause analysis with a controlled experiment (change one thing, measure again) instead of
  trusting the attack's story.
- Reason about defenses: what timer coarsening, cache partitioning and core isolation do and do not stop.

## Safety

Everything here measures cooperating toy programs that you build from this directory (`sites/*.c`). Do
not point the collector at real browsers, other users' processes or machines you do not own.

## Background

**Sweep counting.** The collector (`part2/collector.c`) allocates a buffer about the size of the
last-level cache (LLC; read from `/sys/devices/system/cpu/cpu0/cache`, fallback 8 MiB, clamped to
8 MiB because server LLCs such as the cloud box's 260 MiB would make one sweep longer than a window).
For each window it reads one byte per cache line, over and over, and records the number of complete
sweeps. Anything that slows those sweeps down or takes the CPU away lowers the count: other programs'
cache and memory traffic, being descheduled, interrupts, frequency changes.

**The victims.** `sites/` contains eight small C programs that stand in for websites. Each has a
characteristic timeline built from four primitives in `sites/site.h`: *stream* (read-modify-write
over a large buffer; bandwidth and cache heavy), *chase* (dependent loads over a large random ring;
latency bound), *compute* (register-only integer mixing), and *idle* (sleep). Start offsets, phase
lengths and intensities are randomized on every run (`--seed`), so the classifier must learn shapes.

| site | timeline (roughly) |
|---|---|
| news | 280 ms streaming page load, then an ad refresh (25 ms stream + compute) every ~250 ms |
| social | repeated "scrolls": 70 ms pointer chase + 25 ms compute, then 60-180 ms idle |
| miner | almost continuous compute, 45 ms bursts with ~4 ms yields |
| video | 150 ms buffering, then a frame every 33 ms (7 ms stream + 3 ms compute) |
| docs | short load, then idle with a 2 ms timer every 500 ms |
| webmail | chase + stream load, background poll every ~400 ms, small "keystroke" bursts |
| maps | random-access tile bursts that grow longer over the visit |
| search | small keystroke bursts while "typing", one heavy results load, then quiet |

**Collection.** `tools/collect.py` starts the collector with `--wait-start` (it allocates and warms up
its buffer, then prints `ready`), starts a site (it builds its data structures, prints `ready`), then
releases the collector, so window 0 starts when the site starts. Traces are taken round-robin over the
sites in shuffled order, so slow drifts of the machine do not line up with one label. It writes
`traces/<name>.npz` (`X`: traces x windows, `y`: labels, `sites`, `meta`).

Collector and site both run at `nice -10` when that is permitted (root, or `CAP_SYS_NICE`, which the
lab's compose file grants; otherwise collect.py prints a note and uses nice 0). Because both get the
*same* weight, how they share a CPU with each other is unchanged. Unrelated background jobs (a
compile, another container) just get a much smaller share of that CPU, so they disturb the traces
less. On the 2-vCPU cloud box, with another job keeping one CPU 100% busy, this raised quick-mode accuracy from
about 0.5-0.6 to about 0.83. Disable it with `NICE=0` to see the difference.

**CPU layouts.** Where the collector and the site run matters a lot, and it is one of the variables you
study in Part 3:

| `LAYOUT=` | collector | site | what couples them |
|---|---|---|---|
| `shared` (default) | CPU 0 | CPU 0 | everything: scheduler time-slicing, interrupts, L1/L2, LLC, memory, frequency |
| `split` | CPU 0 | CPU 1 | only what cores share: LLC, memory bandwidth, power/frequency, and on VMs the hypervisor |
| `none` | OS decides | OS decides | whatever the scheduler does |

`shared` is the default because it is robust on a 2-CPU machine. All other CPUs stay free for the
Python driver and the rest of the system, and browser tabs do end up time-sharing cores. On the
2-vCPU x86_64 cloud VM used to develop this lab (KVM, 260 MiB shared LLC, noisy neighbours), `split`
gave close to chance accuracy, while on desktops and Apple silicon, where cores share a modest
L2/LLC, it can work. Measure it on your machine.

**Timers.** Browsers deliberately coarsen `performance.now()` (to tens or hundreds of microseconds, more
in privacy modes, often with jitter) to make attacks like this harder. Part 1 measures how much that
actually costs an attacker who aggregates many accesses.

## Files

```
part1/timer.c                Part 1 (Exercise 1-1)                          <- you work here
part2/collector.c            Part 2 sweep loop (Exercise 2-1)               <- you work here
part2/eval.py                Part 2 classifier (Exercise 2-2)               <- you work here
part3/collector_counter.c    Part 3 counter loop (Exercise 3-1)             <- you work here
common/collector_frame.h     shared collector main(): args, buffer, warm-up, windows, output format
common/fp.h                  clock, pinning, sysfs LLC size, PRNG (uses ../../common/shd.h)
sites/site.h, sites/*.c      the eight victim "websites"
tools/collect.py             runs collector + site pairs, writes traces/*.npz
tools/plot.py                plots (PNG with matplotlib, ASCII otherwise)
tools/fpdata.py              dataset I/O and a numpy-only classification report
check.py                     grader (quick mode)
solutions/                   reference implementations. SPOILERS: same file names as the starter.
report.md                    template for your answers
```

## Build, run, check

```sh
make                    # builds build/timer, build/collector, build/collector_counter, build/sites/*
make check              # grades your code in quick mode, ~2 minutes (exits non-zero until all parts pass)
make check-solution     # grades the reference solution
make clean              # removes build/ and out/;  make distclean also removes traces/
```

On a Mac, from the repository root: `./scripts/lab-make.sh -C lab1-fingerprint check`. Give Docker
at least 2 CPUs. Do not run two collections (or a check and a collection) at the same time, and avoid
heavy work on the machine while collecting (a running build is fine but costs accuracy).

What `make check` does (quick mode):

| part | test | pass condition |
|---|---|---|
| 1 | `timer --selftest` | `coarsen`, `timed_chase`, `single_shot_accuracy` give known answers |
| 1 | `timer --quick` (N up to 10^6) | a smallest measurable N exists for every clock, and native <= 0.1 ms <= 1 ms with 1 ms > native |
| 2 | collector with `-b 1M` vs `-b 8M` | median sweeps per window > 0, and the 1 MiB count is >= 3x the 8 MiB count |
| 2 | your `evaluate()` on a synthetic 4-class dataset | accuracy >= 0.90 with k-NN (and RandomForest if scikit-learn is installed); test set stratified |
| 2 | end to end: 8 sites x 6 traces x 1 s, P = 10 ms, `LAYOUT=shared` | accuracy >= **0.60** (chance is 0.125) |
| 3 | counter collector with `-b 1M` vs `-b 8M` | counts > 0 and independent of the buffer (ratio 0.5-2) |
| 3 | end to end: 8 sites x 4 traces | runs and reports an accuracy (no threshold: interpreting it is Discussion 3-1) |

`LAYOUT=split make check`, `SEED=123 make check` and `NICE=0 make check` are supported.

## Part 1: timer warm-up (C, native)

How many memory accesses must an attacker aggregate before a timer can see the difference between
cache hits and misses? `part1/timer.c` times N dependent loads in a *hot* ring (16 KiB, stays in L1)
and a *cold* ring (64 MiB, random line order, misses the private caches) for N = 1, 3, 10, ...,
10^7, with three clocks: native `clock_gettime`, and the same rounded down to 0.1 ms and 1 ms. For each N it
repeats both measurements many times and computes the best *single-shot* accuracy: given one
measurement, how often does one threshold classify it correctly? N counts as measurable for a clock when
that reaches 90%.

**Exercise 1-1.** Implement the three marked functions in `part1/timer.c`:
(a) `coarsen()`, a clock rounded down to a resolution;
(b) `timed_chase()`, timestamps around N dependent loads (keep the loop from being optimized away or
reordered across the clock reads);
(c) `single_shot_accuracy()`, the best-threshold classifier described in the comment.
Run `./build/timer --selftest`, then `./build/timer` (full table, ~25 s) or `make timer`.

**Discussion 1-1.** Paste your table. For each clock give the smallest measurable N and explain it
with a back-of-the-envelope calculation from the per-load hot/cold latency difference printed by the
program. Compare with a second machine if you have one (Apple silicon VM vs x86: note the
`clock_getres` line).

**Discussion 1-2.** The table uses single measurements. Explain why averaging many coarse-clock
measurements recovers sub-resolution differences (hint: the start of each measurement is random relative to the
clock tick), and what browsers add to stop that. What does Part 1 imply for the window length P and
the buffer size of a sweep-counting collector that only has a 0.1 ms or 1 ms clock?

## Part 2: sweep counting (C + Python)

**Exercise 2-1.** Implement `measure_window()` in `part2/collector.c`: complete sweeps over the buffer
until the deadline, checking the clock between sweeps only. Read `common/collector_frame.h` first: it
parses `-P/-T/-b/-o/--cpu`, allocates and pre-faults the buffer, warms up, computes absolute window
deadlines and writes the trace. Try it directly:

```sh
./build/collector -P 10 -T 1 | head       # idle machine: roughly constant counts
./build/collector -P 10 -T 1 -b 1M | head # smaller buffer, more sweeps
```

**Exercise 2-2.** Implement `stratified_split()`, `knn_predict()` and `evaluate()` in `part2/eval.py`
(numpy only; use scikit-learn's `RandomForestClassifier` when available, k-NN otherwise). The
provided `preprocess()` normalizes each trace by its own 95th percentile and averages groups of four
windows. `main()` repeats `evaluate()` over 10 random stratified splits and prints a classification
report, a confusion matrix and `accuracy: ...`.

Then collect a real dataset (8 sites x 20 traces x 3 s at P = 5 ms takes ~9 minutes) and evaluate it:

```sh
make collect            # -> traces/sweep.npz   (knobs: TRACES= DURATION= PERIOD= BUF= LAYOUT=)
make eval
make plot               # -> out/sweep_traces.png + ASCII strips in the terminal
```

**Discussion 2-1 (P and buffer size).** Collect smaller datasets with different settings, e.g.
`PERIOD=2|5|20` and `BUF=1M|8M|32M` (use `TRACES=8 DURATION=2` to keep it short), and tabulate
accuracy. Explain the trade-offs: resolution in time vs counts per window (quantization), and buffer
size vs sensitivity to *other* programs' cache use vs sensitivity to losing the CPU. Where is the LLC
size on your machine, and does "LLC-sized" matter in the `shared` layout?

**Discussion 2-2 (traces).** Include the plot of three traces per site. For three sites, point to
the features in the trace that correspond to phases in `sites/<name>.c`, and say which phases are
*invisible* to the collector in your layout.

**Discussion 2-3 (errors).** Which sites does your classifier confuse (confusion matrix)? Explain why
from their source code. Which feature or model change helped most? Possible directions: summary
statistics, a magnitude spectrum for periodic sites, k-NN vs RandomForest.

## Part 3: root cause, "There's Always a Bigger Fish" (C + Python)

**Exercise 3-1.** Implement `measure_window()` in `part3/collector_counter.c`: the same collector with
every memory access removed. Count blocks of `BLOCK` empty register-only iterations per window. Check the
disassembly (`objdump -d build/collector_counter`): the inner loop must have no memory operands. On
x86_64 gcc emits something like `sub $0x2,%eax; jne`. The only memory traffic left is the timer read
(the vDSO data page and the `timespec` that `clock_gettime` fills on the stack).

Then run the controlled experiment, one variable at a time:

```sh
make collect-counter && make eval-counter                    # counter, shared layout
make collect LAYOUT=split && make eval LAYOUT=split          # sweep, split layout
make collect-counter LAYOUT=split && make eval-counter LAYOUT=split
```

**Discussion 3-1 (results).** Fill in the 2x2 table of accuracy: {sweep, counter} x {shared, split}, with
the trace counts and machine you used. What does each comparison isolate?

**Discussion 3-2 (why does the counter still work?).** Explain which mechanisms remain for the counter
collector in each layout: scheduler time-slicing (CFS shares the core when the victim is runnable),
interrupts and their handlers (timer ticks, wake-ups, inter-processor interrupts, softirqs; Cook et
al. found interrupts dominate in the browser setting), frequency and power management, and the
hypervisor on a VM. Propose and, if you can, run one experiment that separates two of them. Ideas:
`/proc/interrupts` and `/proc/<pid>/status` (`nonvoluntary_ctxt_switches`) deltas per trace, running the
site under `nice -n 19` or `chrt --idle 0`, a collector that records `getrusage()` involuntary context
switches per window.

**Discussion 3-3 (defenses).** For each defense, say whether it stops the sweep collector, the counter
collector, or neither, in which layout, and at what cost: (a) coarser timers / jitter (use Part 1),
(b) LLC partitioning (Intel CAT, page coloring), (c) never co-scheduling mutually distrusting
processes on a core (core isolation, as in some browser site-isolation and VM schedulers),
(d) randomized or batched interrupt delivery and timer slack, (e) the victim adding cover traffic.
Which one would you deploy in a browser, and why?

## Hints

<details><summary>Part 1</summary>

- Use the result of the chase (`*pos`) so the compiler must perform the loads. Each load must
  depend on the previous one (`p = ring[p]`), otherwise the CPU overlaps them and you measure
  throughput instead of latency.
- `clock_gettime` via the vDSO takes about 20-50 ns. With the native clock even N = 1 can be measurable,
  because one cold load (~100 ns) costs more than the jitter of that overhead.
</details>

<details><summary>Part 2</summary>

- `measure_window()` must not return before the deadline (the frame relies on it), and it must count
  sweeps, not lines.
- If every count is 0 or 1, one sweep takes longer than a window: use a smaller `-b` or a longer `-P`.
- `stratified_split`: shuffle the indices of each class separately with the provided `rng`, take about
  `test_frac` of them (at least 1, at most n-1) for testing.
- k-NN on standardized features: compute mean and standard deviation on the training rows only.
- Accuracy around 0.125 means you have a bug or no signal. Plot first (`make plot`): if the eight
  sites look alike to you, no classifier will separate them.
</details>

<details><summary>Part 3</summary>

- `__asm__ volatile("")` inside the loop prevents the optimizer from deleting the loop without
  introducing memory operands; an `asm` with a `"memory"` clobber would be a different experiment.
- In `shared` layout, think about what the CFS scheduler does when two runnable tasks share one CPU.
</details>

## Going further

- Write the collector in JavaScript (`performance.now()`, an `ArrayBuffer`) and run it in a local HTML page
  against the same toy sites. How much accuracy do you lose with the browser's timer?
- Open-world evaluation: train on six sites, add two unseen ones at test time, and detect them as "unknown".
- Replace RandomForest with a small 1-D CNN on the raw trace, as in the original papers.
- On a machine with SMT, add a `sibling` layout (collector and site on two hyperthreads of one core) and
  compare with `shared` and `split`.
- Add an "interrupt counting" collector that records `/proc/interrupts` deltas per window.

## References

- A. Shusterman, L. Kang, Y. Haskal, Y. Meltser, P. Mittal, Y. Oren, Y. Yarom, "Robust Website Fingerprinting
  Through the Cache Occupancy Channel", USENIX Security 2019.
- J. Cook, J. Drean, J. Behrens, M. Yan, "There's Always a Bigger Fish: A Clarifying Analysis of a
  Machine-Learning-Assisted Side-Channel Attack", ISCA 2022.
- Y. Oren, V. P. Kemerlis, S. Sethumadhavan, A. D. Keromytis, "The Spy in the Sandbox: Practical Cache Attacks
  in JavaScript and their Implications", CCS 2015.
- M. Schwarz, C. Maurice, D. Gruss, S. Mangard, "Fantastic Timers and Where to Find Them: High-Resolution
  Microarchitectural Attacks in JavaScript", Financial Cryptography 2017.
- A. Panchenko et al., "Website Fingerprinting at Internet Scale", NDSS 2016.
