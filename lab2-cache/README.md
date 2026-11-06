# lab2-cache: cache timing, Flush+Reload, Prime+Probe, key leakage, covert channel

This lab builds up the classic cache side-channel toolbox one step at a time. You start by measuring
how long a load takes depending on where the line lives. Then you use that to read a victim's secret
with **Flush+Reload**, find a victim's cache set with **Prime+Probe**, leak a key from a victim's
secret-dependent code paths, and finally build a **covert channel** that carries text between two
programs that share nothing but a CPU core. Each attack is followed by a defense experiment.

Everything runs on a **deterministic simulator** by default, so it works the same on an Apple-silicon
Docker container, an x86_64 laptop or a cloud VM. The *same code* also runs on the real machine with
`SHD_BACKEND=native` (best on x86_64 Linux; see [Native backend](#native-backend)).

**Lecture mapping.** This lab goes with the lecture on cache timing side channels (L03): the memory
hierarchy as a shared resource, the hit/miss timing difference, Flush+Reload, Prime+Probe, eviction
sets, covert vs. side channels, and the main defenses (constant-time code, partitioning,
randomization).

**Learning goals.** After this lab you can:
- measure and explain the latency of each cache level, and choose thresholds from data;
- map addresses to cache sets, and explain what small pages hide and huge pages reveal;
- carry out Flush+Reload and Prime+Probe, and make them robust to prefetchers, replacement-policy
  quirks, noise and a victim that is only sometimes active;
- recover a secret that is encoded in *time* as well as *space*;
- design a small noisy-channel protocol (synchronization, framing, error detection);
- argue about what stops each attack, and what the defense costs.

| Part | Topic | Backend | Est. time |
|---|---|---|---|
| 1 | Latency histograms and thresholds | sim (+ native) | 1-2 h |
| 2 | Flush+Reload CTF | sim (+ native) | 1-2 h |
| 3 | Prime+Probe CTF (easy and hard victim) | sim (+ native, x86_64) | 2-4 h |
| 4 | Key from secret-dependent code paths, then fix the victim | sim (+ native, x86_64) | 2-3 h |
| 5 | Covert "dead drop" channel | sim (+ native, x86_64) | 3-5 h |

> `solutions/` contains reference solutions. **Spoilers.** Try first.

---------------------------------------------------------------------------------------------------

## Quick start

```sh
make                      # builds bin/part1 .. bin/part5 from YOUR code (starter code compiles as-is)
./bin/part1               # run one part (sim backend, random seed)
./bin/part3 --mode hard --seed 42      # parts take options; --seed makes a run reproducible
make check                # grade all parts (sim); make check PARTS=2 for one part; SEED=... to reproduce
make check-solution       # grade the reference solutions (should print Score: 100/100)
SHD_BACKEND=native ./bin/part2 --same-core   # try the real machine
make clean
```

Inside Docker on a Mac: `./scripts/lab-make.sh -C lab2-cache check` from the repository root, or open
a shell with `./scripts/lab-shell.sh` and work in `lab2-cache/`.

Files you edit (and only these):

| File | What goes in it |
|---|---|
| `part1/calibrate.c` | `measure_level()`, `calibrate()` |
| `part2/flush_reload.c` | `fr_attack()` |
| `part3/evset.c`, `part3/prime_probe.c` | eviction sets, prime/probe helpers, `pp_find_set()` |
| `part4/key_leak.c`, `part4/victim_lookup.c` | `leak_key()`, `victim_lookup_ct()` |
| `part5/sender.c`, `part5/receiver.c`, `part5/channel.h` | your protocol |

`include/lab.h` defines every interface and the public facts about each victim. **Read it.** Later
parts reuse earlier ones: Parts 2-5 call your `calibrate()`, and Parts 4-5 can use your Part 3
helpers.

---------------------------------------------------------------------------------------------------

## The platform

Attack code talks to the hardware only through `platform/plat.h`:

| Call | Meaning |
|---|---|
| `plat_geom()` | cache/page geometry (`line`, `l1_sets/ways`, `l2_sets/ways`, `llc_sets/ways`, `page`, `huge`) |
| `plat_alloc(bytes, huge)` | private, page-aligned memory; `huge=1` asks for 2 MiB pages |
| `plat_is_huge(p)` | did we really get huge pages for `p`? |
| `plat_access(p)`, `plat_read8(p)` | one load (no timing) |
| `plat_probe(p)` | one *timed* load: latency in ticks, including timer overhead |
| `plat_flush(p)` | evict the line from all levels of all cores (disabled in Part 5) |
| `plat_fence()` | memory fence |
| `plat_now()` | current time in ticks (one time base shared by all parties, like the TSC) |
| `plat_wait(t)` | do nothing for `t` ticks (this is how the other parties get to run in sim) |
| `plat_rand()` | deterministic PRNG seeded from `--seed` |

Pointers are ordinary pointers. You may build data structures in memory you allocated with plain C;
only `plat_*` accesses are seen by the cache model. Victims and drivers use a separate `harness.h`.
The grader rejects attack code that calls it.

### The simulated machine (`SHD_BACKEND=sim`, the default)

| | |
|---|---|
| Cores | 2. Core 0 has two hardware threads ("harts"): you, and an SMT sibling. Core 1 is another core. |
| L1D | 32 KiB, 8-way, 64 sets, tree-PLRU, private per core (shared by its two harts) |
| L2 | 1 MiB, 16-way, 1024 sets, tree-PLRU, private per core, **inclusive** of L1 |
| LLC | 4 MiB, 16-way, 4096 sets, LRU, shared, **inclusive**: an LLC eviction removes the line from every L1/L2 |
| Lines, pages | 64-byte lines; 4 KiB pages map to **random** physical frames; 2 MiB pages map to random 2 MiB-aligned physical chunks |
| Indexing | set = (physical address / 64) mod sets (no slice hashing) |
| Prefetchers | L2 stride prefetcher per core (trains on L1 misses, after two equal strides inside a 4 KiB page fetches the next two lines); adjacent-line prefetch into the LLC on DRAM fills |
| Latency (cycles) | L1 4, L2 14, LLC 42-48, DRAM ~170-250 (row-buffer and queueing jitter); `plat_probe` adds ~24 +/- 3 cycles of timer overhead; ~0.2% of probes hit an "interrupt" (+300..3300) |
| Noise | an "OS" party per core touches random lines every few thousand cycles and pollutes ~1000 lines at once every ~1M cycles |

**Virtual time.** Every party (your attacker, the victim, the sender, the receiver) is a coroutine
with its own clock. Every `plat_*` call charges cycles to the caller. The scheduler always runs the
party that is furthest behind and switches when the running one gets more than 1000 cycles ahead or
calls `plat_wait()`. Parties therefore behave as if they ran in parallel, interleaved at a fine
grain. Your clock only advances because of your own work; a victim doing something does not make
your `plat_now()` jump. Runs are reproducible: the same seed gives the same run, bit for bit.

**Knobs** (environment variables) for experiments:

| Variable | Effect |
|---|---|
| `SHD_SIM_NOISE=0..3` | background noise level (default 1) |
| `SHD_SIM_REPL=lru\|plru\|random` | replacement policy for all levels |
| `SHD_SIM_PREFETCH=0` | turn the prefetchers off |
| `SHD_SIM_PARTITION=1` | way-partition L1 and L2 between the two harts of a core (Part 5 defense) |
| `SHD_SIM_NOHUGE=1` | `plat_alloc(.., 1)` returns small pages ("Going further") |
| `SHD_SIM_QUANTUM=N` | scheduling granularity in cycles (default 1000) |
| `SHD_SIM_STATS=1` | print access/switch statistics at exit |

### Address bits

With 64-byte lines, bits 0-5 of an address select the byte within the line. The next
log2(sets) bits select the set; the rest are the tag. Only the bits *below the page size* are the
same in the virtual and the physical address. In the sim:

| Cache | Set-index bits | Known with 4 KiB pages? |
|---|---|---|
| L1 (64 sets) | 6-11 | all (inside the page offset) |
| L2 (1024 sets) | 6-15 | bits 6-11 only; bits 12-15 are unknown, so there are 16 possible sets ("colors") |
| LLC (4096 sets) | 6-17 | bits 6-11 only |

Inside a 2 MiB page, bits 0-20 are known, so you can compute the L2 set of any line yourself.

---------------------------------------------------------------------------------------------------

## Part 1: latency histograms and thresholds (sim + native, 1-2 h)

`./bin/part1 [--samples N] [--csv out.csv]` calls your `measure_level(level, samples, n)` for each
level, prints a summary and a text histogram, then calls your `calibrate()`. In sim, the driver
compares the result against the model's true latency distributions.

**Exercise 1-1.** Implement `measure_level()`. For each sample, put one line exactly at the requested
level, then time one load of it with `plat_probe()`:
- **L1**: touch it, then time it.
- **L2**: touch it, then evict it from L1 *only*. Use lines that share its L1 set but not its L2 set.
- **LLC**: touch it, then evict it from L2. Use more than `l2_ways` lines congruent in L2. Because
  the L2 is inclusive, this also removes it from L1.
- **DRAM**: touch it, then `plat_flush()` it.

**Exercise 1-2.** Implement `calibrate()`: medians per level, plus two thresholds. `l2_miss` separates
{L1, L2} from {LLC, DRAM}, and `llc_miss` separates {L1, L2, LLC} from DRAM. Choose each threshold
so that it separates the two distributions with the largest margin.

**Discussion 1-3.** Fill in a geometry table for the simulated machine *and* for your own machine
(`lscpu`, `/sys/devices/system/cpu/cpu0/cache/index*`): size, ways, sets, which address bits are the
offset, the index and the tag, and how many index bits a 4 KiB page hides. How many lines do you need
to evict a line from L1, from L2, and from the LLC, and how did you pick them?

**Discussion 1-4.** Paste your histograms (or plot the CSV with `python3 tools/plot_hist.py
out.csv`). Which levels overlap? Where do the long tails come from? What happens to the histograms
with `SHD_SIM_NOISE=3`? If you can, run `SHD_BACKEND=native ./bin/part1` and compare. On aarch64,
also try `SHD_TIMER=thread`.

*Hints.* Choose a different random line for every sample. A fixed or sequential pattern trains the
stride prefetcher, and then "DRAM" samples are served from L2. Tree-PLRU may need more than `ways`
misses (or a second pass) before a particular line is gone. Do the preparation with `plat_access`,
and use `plat_probe` only for the one measurement.

---------------------------------------------------------------------------------------------------

## Part 2: Flush+Reload CTF (sim + native, 1-2 h)

`./bin/part2 [--same-core]` starts a victim on the **other core**. The victim keeps loading
`shared + secret * FR_STRIDE` for a secret slot in `[0, 1024)`, and now and then also touches a random
decoy slot. `shared` is mapped read-only into both processes, like a page of a shared library.
`FR_STRIDE` is 128 bytes in sim (one page natively, see below).

**Exercise 2-1.** Implement `fr_attack()`. Each round: flush every slot, `plat_wait()` so the victim
can run, then reload every slot with `plat_probe()` and count the fast ones. A slot that comes back
fast was brought back into the cache by someone. Vote over rounds and stop when one slot clearly
dominates.

**Discussion 2-2.** Change your reload loop to visit the slots in order 0, 1, 2, ... and run it again.
Then run it again with `SHD_SIM_PREFETCH=0`. Explain what you see, and why randomizing the reload
order fixes it. Why are the slots 128 bytes apart and not 64? Natively the slots are a page apart:
real Intel L2 "streamer" prefetchers are aggressive enough within a 4 KiB page that even a shuffled
order breaks a 128-byte stride.

**Discussion 2-3.** What exactly does Flush+Reload need from the system (shared memory, a flush
instruction or an equivalent eviction method, a timer)? For each, name a defense that removes it and
what it costs: no page deduplication, copy-on-access for shared libraries, restricting `clflush`,
coarse timers. Why does cache partitioning (Part 5) *not* stop Flush+Reload? How would you change
the victim so that it leaks nothing here?

*Hints.* A threshold calibrated on *your* core may not fit a line that sits in the *other* core's
cache. On real hardware with a non-inclusive LLC, that reload is a cross-core snoop. A threshold
relative to each round (for example, the fastest few slots) is more robust.

---------------------------------------------------------------------------------------------------

## Part 3: Prime+Probe CTF (sim + native, 2-4 h)

`./bin/part3 --mode easy|hard` starts a victim on your **SMT sibling**, so it shares your L1 and L2.
The victim hammers lines that all map to **one secret L2 set**, and you must find that set's index.
You cannot see the victim's memory, so the only signal is that your own lines in that set keep
getting evicted.

- **easy**: 16 lines, all the time.
- **hard**: only 4 lines, and only in **bursts** (active about 25% of the time, with a period of about
  100k ticks in sim). It also does more unrelated work.
- For experiments: `--lines N` sets the number of victim lines, and `--steady` turns bursts off.

**Exercise 3-1.** Implement `evset_build()`, `pp_prime()` and `pp_probe()` in `part3/evset.c`. The
pool comes from huge pages, so congruent lines are simply `l2_sets * line` bytes apart.

**Exercise 3-2.** Implement `pp_find_set()` and pass easy mode. Recipe: for every set, prime, wait,
probe, and count misses. Scan everything quickly, then re-measure the best candidates for longer.

**Exercise 3-3.** Pass hard mode.

**Discussion 3-4.** Record a histogram of "misses per probe" for the victim's set and for a quiet set
(a) with a single forward prime and (b) with a forward-then-backward prime. Explain the difference
with the replacement policy: what is the probe *cascade*? Repeat with `SHD_SIM_REPL=lru`. What does
this mean for choosing a detection rule?

**Discussion 3-5.** Run with `SHD_SIM_NOHUGE=1` (no huge pages). Which bits of the victim's set index
can you still learn, and why can you not learn the rest? Sketch, or implement, how to build an L2
eviction set from small pages by timing alone (group testing / reduction; Vila et al.).

*Hints.* One eviction anywhere in a set can make a naive probe report *every* line missing, so a
single round tells you little. For a bursty victim, look at every set at several different times. Do
not scan in the same order every pass: a pass takes an almost constant time, so a fixed order can
visit the victim's set at the same burst phase every time. Watch the finalists for longer than one
burst period.

---------------------------------------------------------------------------------------------------

## Part 4: a key from secret-dependent code paths (sim + native, 2-3 h)

`./bin/part4 [--bits K]` starts a victim on your SMT sibling. Its "binary layout" is public (see
`include/lab.h`, `KEY_*`). It has two lookup tables, `table_zero` and `table_one`, each in its own
4 KiB page at a known page offset. For every key bit it repeatedly reads `table_one` if the bit is 1
and `table_zero` if it is 0, for `KEY_SLOT_TICKS`. Between rounds over the key it idles for
`KEY_GAP_SLOTS` slots. You know each table's page offset but not its physical page, so each table can
be in any of 16 L2 sets (32 natively on a 2048-set L2).

**Exercise 4-1.** Find the L2 set (color) of each table: watch every candidate long enough to cover a
whole key period.

**Exercise 4-2.** Implement `leak_key()`: watch both sets over several periods, turn activity into a
bit sequence, use the idle gap to find where the key starts, and output the bits. The grader uses a
2-bit key; try `--bits 16`.

**Exercise 4-3 (defense).** Implement `victim_lookup_ct()` in `part4/victim_lookup.c`. It must return
the same value as `victim_lookup()`, but the sequence of cache lines it touches must not depend on
the bit: no secret-dependent addresses and no secret-dependent branches. `./bin/part4 --ct-check`
grades it from the simulator's access trace. Then run your attack against the fixed victim with
`./bin/part4 --defend --bits 16`.

**Discussion 4-4.** How did you tell the real table set from look-alikes? Hint: with tree-PLRU a
*foreign line can get stuck* in a primed set, and then your own probe misses every round for a long
time. What property of the victim's activity did your scoring rely on (busy *and* idle? repeating
with the key period?)

**Discussion 4-5.** What does the constant-trace fix cost here, and what would it cost for a real
table-based cipher (e.g. AES T-tables: how many lines would every lookup have to touch)? Why is
"touch every line" not always enough (cache-bank conflicts, CacheBleed)? Name the alternatives
(bitslicing, hardware AES).

*Hints.* Watch the four sets of a table's lines together. Bucket your observations by the slot they
fall into (`plat_now()` is shared). For a 2-bit key, a candidate whose activity looks the same in two
consecutive key periods is the real one.

---------------------------------------------------------------------------------------------------

## Part 5: covert "dead drop" (sim + native, 3-5 h)

`./bin/part5 [--msg "text"] [--len N]` starts your **receiver** (SMT sibling) and then your **sender**
(the other hart of the same core) as separate parties. Natively they are separate processes. They
share **no writable memory**, and `plat_flush()` aborts. The sender gets a message of arbitrary
length; the receiver must return exactly that text. Both get a copy of the calibration that the
driver ran with your `calibrate()`.

**Exercise 5-1.** Design and implement the protocol in `part5/sender.c`, `part5/receiver.c` and
`part5/channel.h`.
- *Physical layer*: the sender fills agreed L2 sets (use more lines than ways) in agreed time slots;
  the receiver Prime+Probes them. `plat_now()` is a shared clock.
- *Bit encoding*: think about what makes a 1 distinguishable from a 0 *without* a fixed threshold.
- *Framing*: a start marker, a length or end marker, and a checksum (CRC).
- *Robustness*: noise bursts happen; repeat frames or add error correction. Give up eventually.

**Discussion 5-2.** Describe your protocol. Measure the bit rate (payload bits per million ticks; the
driver prints it) and the frame error rate for at least three slot lengths, and plot or tabulate the
trade-off. At 3 GHz, how many bits per second is that?

**Discussion 5-3 (defense).** Run your channel with `SHD_SIM_PARTITION=1` (each hart may only fill its
own half of the L1/L2 ways). What happens, and why? What does way-partitioning cost a normal program
(effective cache size; measure it with Part 1 if you like)? Which channels does it leave open (the
shared LLC, Flush+Reload on shared data)? Compare with set-randomizing caches (CEASER, ScatterCache),
with disabling SMT, and with noise injection.

*Rules.* Talk only through the cache. The grader inspects `sender.o` and `receiver.o` with `nm`:
they must not reference each other's symbols, and must not use files, pipes, sockets, shared memory,
signals, environment variables or `fork`/`exec`.

---------------------------------------------------------------------------------------------------

## Native backend

`SHD_BACKEND=native ./bin/partN` uses real memory and the timer from `common/shd.h`: `rdtscp` and
`clflush` on x86_64, `cntvct_el0` and `dc civac` on aarch64. Victims are real processes created with
`fork()` and pinned with `sched_setaffinity`. The Flush+Reload buffer is a `memfd` mapped read-only
`MAP_SHARED` into both processes. `HX_SIBLING` placement uses the SMT sibling of CPU 0 when there is
one. Otherwise the victim shares CPU 0 itself: it is time-sliced, and `plat_wait()` calls
`sched_yield()` so that the two processes alternate. Huge pages come from hugetlbfs if its pool has
pages, otherwise from 2 MiB-aligned transparent huge pages (`madvise`); a warning is printed if
neither works. Physical addresses (used only by the Part 3 victim) need root for
`/proc/self/pagemap`; without it the victim falls back to huge-page virtual address bits.

What to expect:
- **x86_64 bare metal with SMT**: the intended setting for every part. Expect to tune waits and
  thresholds, since the timer runs in TSC ticks.
- **x86_64 VM (measured on a 2-vCPU KVM guest of a Xeon "Sapphire Rapids"-class host, no SMT, THP
  available)**:
  - Part 1 orders the levels correctly (5/5). L1 and L2 are close (~70 vs ~78 ticks, because the
    `rdtscp` overhead dominates); the LLC is ~130 and DRAM ~265.
  - Part 2 works reliably with `--same-core` (5/5). Across cores it works only 1-3/5: the LLC is
    non-inclusive, so the victim's line comes from a cross-core snoop at ~246 ticks, barely faster
    than DRAM at ~266.
  - Parts 3-5 mostly fail (Part 3 easy 0-2/5, hard 0/5, Part 4 0-1/5; Part 5 occasionally decodes
    an 8-byte message but not the graded lengths). The guest's huge pages are physically contiguous
    *in the guest*, but evidently not always in the host, so L2 congruence computed from address
    bits holds in some runs and not in others. The real L2 is also not LRU: we measured that it takes
    about 2x `ways` congruent lines to evict a line reliably.
- **aarch64 (Apple silicon VM/Docker)**: `dc civac` works, but single-access timing is too coarse
  (`cntvct_el0` is 24 MHz), and the counting-thread timer (`SHD_TIMER=thread`) shows no hit/miss gap
  in a VM. Use the sim backend there. The native backend still runs, which is useful for checking
  that your code is backend-agnostic.

Write geometry-agnostic code: use `plat_geom()`, your calibration, and the `FR_STRIDE` and
`KEY_SLOT_TICKS` macros, which are larger natively. Then the same attack code runs on both backends.

---------------------------------------------------------------------------------------------------

## Grading

`make check` runs `check.py`. Each part runs 5 trials with fresh secrets (the seed is printed, so
`make check SEED=...` reproduces a run). A part passes with at least 4 of 5 trials; the
constant-trace check needs 3 of 3:

| Item | Points |
|---|---|
| Part 1 thresholds (vs. the model's true distributions) | 10 |
| Part 2 Flush+Reload | 15 |
| Part 3a Prime+Probe, easy | 15 |
| Part 3b Prime+Probe, hard | 10 |
| Part 4a 2-bit key | 15 |
| Part 4b constant-trace lookup | 10 |
| Part 5 covert channel (random messages of about 25-55 characters) | 25 |

Before running, the grader inspects your object files with `nm`. Attack code must not use
`harness.h` or simulator internals, and the Part 5 rules above apply. The reference solutions take
about 10 s in total; every trial has a 150 s limit. `python3 check.py -v` shows each trial's output,
and `python3 check.py --backend native` tries the native backend (informational only).

## Going further

- **No huge pages.** With `SHD_SIM_NOHUGE=1`, build L2 eviction sets from 4 KiB pages using timing
  only (group-testing reduction), then redo Part 3 and report the set index modulo what you can know.
- **LLC Prime+Probe across cores.** Put the Part 3 victim on the other core (edit a copy of the
  driver) and attack through the inclusive LLC. How many lines does an LLC eviction set need, and how
  many candidates are there?
- **Replacement policy reverse-engineering.** Design access sequences that tell LRU, tree-PLRU and
  random apart (try each `SHD_SIM_REPL`), then try them natively.
- **Faster channel.** Use several sets as parallel lanes, shorter slots, or forward error correction
  (Hamming or repetition codes) instead of retransmission. What limits the rate?
- **Flush+Flush / Evict+Reload.** Implement the variants and compare their noise and speed with
  Flush+Reload.

## References

- D. Gullasch, E. Bangerter, S. Krenn. *Cache Games: Bringing Access-Based Cache Attacks on AES to
  Practice.* IEEE S&P 2011.
- Y. Yarom, K. Falkner. *FLUSH+RELOAD: a High Resolution, Low Noise, L3 Cache Side-Channel Attack.*
  USENIX Security 2014.
- D. A. Osvik, A. Shamir, E. Tromer. *Cache Attacks and Countermeasures: the Case of AES.* CT-RSA 2006.
- C. Percival. *Cache Missing for Fun and Profit.* BSDCan 2005.
- F. Liu, Y. Yarom, Q. Ge, G. Heiser, R. B. Lee. *Last-Level Cache Side-Channel Attacks are
  Practical.* IEEE S&P 2015.
- P. Vila, B. Köpf, J. F. Morales. *Theory and Practice of Finding Eviction Sets.* IEEE S&P 2019.
- C. Maurice et al. *Hello from the Other Side: SSH over Robust Cache Covert Channels in the Cloud.*
  NDSS 2017.
- D. Gruss, C. Maurice, K. Wagner, S. Mangard. *Flush+Flush: A Fast and Stealthy Cache Attack.*
  DIMVA 2016.
- Y. Yarom, D. Genkin, N. Heninger. *CacheBleed: A Timing Attack on OpenSSL Constant Time RSA.*
  CHES 2016.
- F. Liu et al. *CATalyst: Defeating Last-Level Cache Side Channel Attacks in Cloud Computing.* HPCA
  2016.
- V. Kiriansky et al. *DAWG: A Defense Against Cache Timing Attacks in Speculative Execution
  Processors.* MICRO 2018.
- M. K. Qureshi. *CEASER: Mitigating Conflict-Based Cache Attacks via Encrypted-Address and
  Remapping.* MICRO 2018.
- M. Werner et al. *ScatterCache: Thwarting Cache Attacks via Cache Set Randomization.* USENIX
  Security 2019.
