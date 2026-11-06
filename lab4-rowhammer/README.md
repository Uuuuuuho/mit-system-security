# Lab 4: From virtual addresses to DRAM disturbance (Rowhammer)

In this lab you work through the whole Rowhammer chain, starting from a virtual address in your
own process and ending with flipped bits and the defenses that stop them:

1. **Part 1** turns virtual addresses into physical ones and back again (`/proc/self/pagemap`, huge pages).
2. **Parts 2 and 3** use timing to discover how physical addresses map onto DRAM banks and rows.
3. **Part 4** uses that knowledge to hammer rows: single-sided, double-sided and many-sided, with
   and without an in-DRAM TRR sampler.
4. **Part 5** builds SECDED ECC and measures what it can and cannot fix.
5. **Part 6** puts a Rowhammer mitigation (PARA, Graphene) into the memory controller and measures
   how well it protects and what it costs.

| Part | Topic | Backend | Est. time |
|------|-------|---------|-----------|
| 1 | virt → phys → virt | **native** when PFNs are visible, else a **simulated pagemap** | 1–1.5 h |
| 2 | bank conflicts by timing | simulator (`dramsim`) | 1 h |
| 3 | bank XOR-function recovery (easy + challenge) | simulator | 2–3 h |
| 4 | single / double / many-sided hammering, TRR | simulator | 2–3 h |
| 5 | Hamming(39,32) SECDED | pure C + simulator demo | 1–1.5 h |
| 6 | PARA and Graphene in the memory controller | simulator | 2–3 h |

**Safety.** Disturbance errors happen only inside `dramsim`, a DRAM model written for this lab.
Nothing in this repository hammers real memory, and you should not add code that does. Part 1 only
reads page-table metadata of *your own* process.

**Mapping to the course.** This lab goes with the DRAM and Rowhammer lectures (L08–L09 in the
suite's lab map): DRAM organization, row buffers and their timing side channel, address mapping,
refresh, the disturbance mechanism, exploitation prerequisites, TRR, ECC, and controller-side
mitigations. It is an original lab inspired by the MIT 6.5950 Rowhammer lab. Its text, code and
parameters are our own.

**Learning goals.** By the end you should be able to:
- explain why physical addresses matter to an attacker and how Linux restricts them;
- turn row-buffer timing into a same-bank/different-row oracle;
- recover XOR bank functions with GF(2) linear algebra;
- design hammering patterns and reason about refresh windows, thresholds, data patterns and cell
  orientation;
- explain why small TRR samplers fail against many-sided patterns;
- implement SECDED and say exactly where it stops helping;
- size PARA and Graphene from datasheet numbers and measure what they cost.

---------------------------------------------------------------------------------------------------

## Background

### DRAM organization

```
  core -- caches -- MEMORY CONTROLLER === channel: cmd/addr bus + 64-bit data bus ==+
                    | open-page policy, scheduling,                                 |
                    | refresh (REF every tREFI), Part 6 mitigation                  |
                                                                                    v
  DIMM --- rank 0 (8 x8 chips in lockstep, each supplies 8 of the 64 data bits) ---+
       \-- rank 1                                                                  |
                                                                                   v
  one chip: 16 banks (4 bank groups x 4 banks); each bank is an independent array:

                bank b                      cells: capacitor + access transistor
          +-------------------------------+  true cell: charged = 1, anti cell: charged = 0
  row r+2 | . . . . . . . . . . . . . . . |
  row r+1 | # # # # # # # # # # # # # # # |  <- aggressor: opened (ACT) and closed (PRE) again and again
  row r   | . . . . . . x . . . . . x . . |  <- victim: charge leaks faster -> bit flips (x)
  row r-1 | # # # # # # # # # # # # # # # |  <- aggressor
  row r-2 | . . . . . . . . . . . . . . . |  <- distance 2: weaker coupling
          |     (32768 rows per bank)     |
          +---------------+---------------+
                          |  ACT copies one whole row into the row buffer
          +---------------v---------------+
          |  row buffer: 8 KiB per bank   |  READ/WRITE move 64-byte bursts (columns)
          +-------------------------------+  PRE writes the row back and closes the buffer
```

Every bank keeps at most one row open in its **row buffer**. If you access the open row you get a
**row hit** (fast, `tCL`). If the bank is idle and precharged, the controller has to activate the
row first (**closed**, `tRCD + tCL`). If a *different* row is open you get a **row conflict**: the
controller precharges, activates and then reads (`tRP + tRCD + tCL`, the slowest case). Accessing
two addresses alternately that are in the **same bank but different rows** therefore produces a
conflict on every access. That is the timing signal used in Parts 2 and 3 (DRAMA, Pessl et al.).

**Address mapping.** The controller splits a physical address into channel, rank, bank, row and
column. To spread traffic across banks, Intel-style controllers compute each bank bit as the XOR of
several address bits. For example, `bank0 = a13 ^ a18` means that bit 0 of the bank number is
physical-address bit 13 XOR bit 18. The row index is usually a run of the highest address bits.
Vendors do not document these functions, so attackers reverse-engineer them.

```
  one possible layout (the "ddr3-classic" family, 2 ranks = 8 GiB = 33 address bits):

   32                        18 17      13 12               6 5        0
  +----------------------------+----------+------------------+----------+
  | row (15 bits)              | pivots   | column (high)    | in line  |
  +----------------------------+----------+------------------+----------+
     low row bits (a18, a19, ...) XOR pivots a13..a17  ==>  bank bits, e.g. a13^a20, a14^a18 (secret)
```

**Refresh.** Cells leak, so every row has to be refreshed within the refresh window `tREFW`. The
controller issues a `REF` command every `tREFI`. Each REF refreshes a group of rows in every bank,
blocks the rank for `tRFC`, and closes all open rows. Activating a row also restores its charge.

**Rowhammer.** Activating a row over and over disturbs the cells of the neighbouring rows. If a
victim row receives enough activations of its neighbours before it is refreshed, its weakest
cells flip (Kim et al., ISCA 2014). Several things decide whether that happens:
- **thresholds** differ from row to row and from cell to cell;
- **distance**: rows at distance 1 are disturbed strongly, rows at distance 2 more weakly
  ("blast radius");
- **data pattern**: a flip is likelier when the aggressor bit differs from the victim bit;
- **cell orientation**: a *true* cell stores 1 as charge and can only flip 1→0, an *anti* cell
  can only flip 0→1.

Double-sided hammering puts the victim between two aggressors, so every activation hits it.
Single-sided hammering needs a far "dummy" row to force conflicts, which wastes half of the
activations.

**TRR (Target Row Refresh).** DDR4 chips contain an undocumented sampler that tries to identify
aggressor rows and refresh their neighbours "for free" during REF. TRRespass (Frigo et al., S&P
2020) showed that these samplers can track only a few rows. Hammering more aggressors than the
sampler holds ("many-sided" patterns) brings the flips back. U-TRR and Blacksmith later took
vendor samplers apart in detail.

**Defenses.** ECC corrects or detects a few flipped bits per word. PARA refreshes neighbours with a
small probability on every activation. Graphene counts activations with a Misra–Gries heavy-hitter
table and refreshes the neighbours of any row that gets close to the threshold. That gives a
deterministic guarantee for a bounded table size.

### The simulated module (`dramsim/`)

`dramsim` is a deterministic, seedable model of one DDR4-like module behind an open-page
controller. Every `make check` draws a fresh seed, so hard-coded answers do not survive.

| Public ("datasheet", `dsim_info()`) | Secret (you discover it) |
|---|---|
| 4 or 8 GiB; 16 or 32 banks; 32768 rows of 8 KiB per bank | which XOR bank functions (3 realistic families, random partners) |
| your buffer: 1 GiB, physically contiguous, 1 GiB-aligned | where the row bits start |
| `tREFI` = 7.8 µs, `tREFW` = 1024 × tREFI ≈ 8 ms (*shortened*, see below), `tRFC`, `tRC` | exact latencies (jittered ±10 %), CPU clock, noise level |
| `mac` = 12000: no flip below this much effective disturbance | per-row thresholds above `mac`, weak-cell positions, orientation blocks |
| blast radius 2, distance-2 weight ≤ `w2_max` = 0.25 | actual distance-2 weight, data-pattern factor |
| `max_act_per_window` = tREFW / tRC | TRR sampler size, how often TRR acts, its count threshold |

Model rules (the full story is in `dramsim/dramsim.c`):
- **Mapping**: `bank bit i = parity(pa & fn[i])`, `row = pa >> row_lo`, and the column is formed by
  the 13 remaining low bits. Each function has a private "pivot" bit below the row bits. The
  functions come from one of three families modelled on published reverse-engineering results:
  *ddr3-classic* (bank bits just below the row bits, each XORed with a low row bit, like the Sandy
  and Ivy Bridge results in DRAMA), *ddr4-bankgroup* (one very low bit in a bank-group function and
  one three-term function), and *hashed* (one wide, channel-hash-like function that mixes column
  bits with many row bits).
- **Timing**: hit / closed / conflict latencies plus a fixed path overhead, Gaussian noise, rare
  interrupt-like spikes, REF stalls, and background traffic that occasionally opens another row
  in a random bank. Every `dsim_access` is an uncached load, as if you had flushed the line first.
  Latencies are reported in CPU cycles.
- **Disturbance**: each activation of row A adds `w(d) × f` to every weak cell of rows A±1 (`w`=1)
  and A±2 (`w`=w2) that currently holds its charged value. `f` is 1 if the aggressor's bit at the
  same position differs from the victim's bit, otherwise a smaller factor. A cell flips when its
  accumulator reaches its threshold. Accumulators reset when the row is refreshed or activated.
  Refreshes issued by TRR or by your mitigation are activations too, so they disturb *their*
  neighbours (Half-Double, Kogler et al. 2022).
- **TRR** (`opts.trr = 1`): a small per-bank table of `(row, count)` entries with LRU replacement.
  Every few REFs, TRR refreshes the neighbours of one entry whose count passed a threshold, then
  clears the table. This is a simplified model in the spirit of what TRRespass and U-TRR observed
  in real samplers, not a copy of any vendor's design.
- **Time scale**: the refresh window is 8 ms instead of 64 ms, and the thresholds are scaled to
  match (a window holds ≈ 80–90 k activations). That keeps every experiment under a second while
  the ratios that matter stay realistic.

Use only `dramsim/dramsim.h` in your code. `dramsim/dramsim_oracle.h` is the grader's ground truth.

---------------------------------------------------------------------------------------------------

## Getting started

```
make                 # builds build/part1 .. build/part6 from YOUR code in src/
./build/part2 --seed 7 -v
make check           # grades all parts (fresh seed each time; SEED=1234 make check to repeat)
make check PARTS="2 3"
make check-solution  # grades the reference solution (solutions/ - SPOILERS)
make clean
```

You edit only the six files in `src/` (one per part). Prototypes and documentation are in
`include/lab4.h`. Each harness program in `harness/` calls your functions, prints what happened,
and ends with `RESULT part=N status=PASS|FAIL`. `check.py` collects these lines. The harnesses take
`--seed N`, `-v` (more output) and `--reveal` (prints the ground truth after the run, for debugging
only).

Everything runs on x86_64 and aarch64 Linux (Docker on Apple silicon works) and needs only gcc or
clang, make and python3. With the repository's `compose.yaml`, the container has `CAP_SYS_ADMIN`,
so Part 1 can usually run natively.

**Spoiler warning:** `solutions/` contains complete reference solutions with the same file names
as `src/`. Look only after you have tried.

---------------------------------------------------------------------------------------------------

## Part 1: virtual → physical → virtual (native or simulated pagemap)

Rowhammer works on *physical* rows, but your program only sees virtual addresses. Linux exposes one
64-bit entry per virtual page in `/proc/self/pagemap`: the page-frame number (PFN) and some flags.
Since Linux 4.0 the PFN field reads as zero unless you have `CAP_SYS_ADMIN`. That restriction was a
direct response to Rowhammer exploits.

`support/pagemap.h` gives you *raw* entries from one of two backends:
- **native**: `pread()` on `/proc/self/pagemap`;
- **sim**: the same 64-bit format, but the PFNs come from a simulated kernel with 16 GiB of fake
  memory. The present bit is still the real one. Huge-page buffers get contiguous, aligned fake
  frames, and every other page reads PFN 0, as it would for an unprivileged process.

`pagemap_open(PM_AUTO, …)` probes whether PFNs are visible and falls back to sim if they are not
(the reason is printed). `--mode native|sim|auto` and `SHD_PAGEMAP=native|sim|auto` select the
backend explicitly. `make check` always grades the sim mode and also runs auto mode, which uses
native when PFNs are visible.

- **Exercise 1-1.** Implement `pm_decode()` (present, swapped, PFN field, PFN 0 means "hidden",
  page offset) and `pm_virt_to_phys()`. Read the kernel's `pagemap.rst` for the bit layout. Watch
  out for the flag bits right above the PFN field.
- **Exercise 1-2.** Implement the reverse map `revmap_build()` / `revmap_phys_to_virt()` for one
  buffer. It must return NULL for physical addresses outside the buffer.
- **Exercise 1-3.** Implement `revmap_count_huge()`: count the huge-aligned chunks whose pages are
  physically contiguous and start at a huge-aligned physical address. The harness allocates the
  buffer with `pagemap_alloc(.., huge=1)`, which uses hugetlbfs if configured and otherwise
  `madvise(MADV_HUGEPAGE)`.
- **Discussion 1-1.** Why does hiding PFNs make Rowhammer exploitation harder, and how do attackers
  get by without them? Consider huge pages (how many low physical bits do you learn?), memory
  spraying, and timing.
- **Discussion 1-2.** What did auto mode do on your machine (native or sim, and why)? With native
  PFNs, how many of the huge chunks were really contiguous? Why is a transparent huge page never
  guaranteed?

Checks: decoding of hand-made entries; 64 translations; untouched and PFN-less pages are rejected;
2000 reverse-map round trips; NULL outside the buffer; the huge-chunk count matches.

## Part 2: bank conflicts by timing (simulator)

From here on you work with physical addresses inside your 1 GiB buffer in `dramsim`, as if you had
already run Part 1 on a 1 GiB huge page.

- **Exercise 2-1.** `measure_pair(sim, a, b, rounds)`: access a, b, a, b, … and return a *robust*
  latency summary. Think about warm-up, refresh stalls, spikes, and background traffic.
- **Exercise 2-2.** `calibrate_threshold()`: measure many random pairs, look at the bimodal
  distribution (the harness prints a histogram of your measurements), and pick a threshold using
  timing only.
- **Exercise 2-3.** `same_bank_diff_row()`: a reliable conflict oracle. Parts 3 and 4 depend on it.
- **Discussion 2-1.** Show the histogram and your threshold. Which noise sources did you see, and
  how many accesses per decision did you need for ≥ 99 % accuracy?
- **Discussion 2-2 (defense).** Would a closed-page or timeout-based page policy remove this
  channel? What would it cost in performance? Are there other ways to see bank conflicts (e.g.
  from another core)?

Checks: conflict pairs are > 30 cycles slower than the other pairs; the threshold lies between
the populations; ≥ 99 % accuracy and ≥ 95 % recall and precision on 1800 pairs.

## Part 3: recovering the bank functions (simulator)

- **Exercise 3-1 (easy).** `pick_candidate()`: `dsim_candidates()` gives three candidate function
  sets, and exactly one is real. Build address pairs that a candidate predicts to be same-bank and
  test them. Hint: flipping a candidate's pivot bit changes exactly one bank bit.
- **Exercise 3-2 (challenge).** `recover_bank_fns()` gets no candidates. Collect addresses that
  conflict with a base address `a0`. Every bank function `f` satisfies
  `parity(f & (a0 ^ x)) = 0` for all of them, so the functions span the **null space over GF(2)**
  of the difference vectors, restricted to the bits that vary inside your buffer. Return any
  basis; the grader compares spans. Bonus: output the lowest-weight basis in DRAMA notation.
- **Exercise 3-3.** `find_row_lo()`: flip one bit, flip pivot bits so that the bank stays the same,
  and test for a conflict. The lowest bit that causes one is the lowest row bit.
- **Discussion 3-1.** Write your recovered functions in DRAMA notation. Which family do they
  resemble? Which bits of the true functions can you *not* see from inside your buffer, and why
  does that not matter to an attacker confined to it?
- **Discussion 3-2.** How many conflict tests did you need? Compare brute force over masks with
  the linear-algebra approach. Why does a single false positive break the null space, and what did
  you do about it?

Part 3 uses your Part 2 code (`calibrate_threshold`, `same_bank_diff_row`). Checks: the right
candidate; the right span with the right number of functions; the right `row_lo`. If 3-2 fails,
3-3 is given the true functions so it can still be graded.

## Part 4: hammering in the simulator

The harness gives you the true mapping (`dram_map_t`) so that this part does not depend on Part 3.
(Going further: plug in your own Part 3 output.) Two module instances are used: one without TRR and
one with TRR.

- **Exercise 4-1.** `map_compose(m, bank, row, col)`: build a physical address from row bits and
  column bits, then fix each bank bit with its pivot.
- **Exercise 4-2.** `hammer_once()`: fill victims with `pattern` and aggressors with `~pattern`,
  hammer round-robin for `duration_ns`, then read the victims back and count flips (1→0 and 0→1
  separately). Handle single-sided (aggressor plus a far dummy row), double-sided, and many-sided
  (`row, row+2, …`) patterns. The harness hammers for two refresh windows. Why two?
- **Exercise 4-3.** `min_sides_under_trr()`: with TRR on, find the smallest number of aggressors
  that still flips bits. Test several locations and both data polarities.
- **Discussion 4-1.** Show the success table printed by the harness. Explain single- vs double- vs
  many-sided without TRR, why the 1→0 and 0→1 counts differ between locations, and what a weaker
  data pattern (e.g. victims = aggressors, or 0x55…/0xAA…) does.
- **Discussion 4-2.** What did Exercise 4-3 reveal about the TRR sampler? Why do many-sided
  patterns defeat it, and why did real-world samplers need the non-uniform patterns of Blacksmith
  instead of plain many-sided ones?
- **Discussion 4-3 (defense).** How would you change the sampler so that your pattern fails
  (table size, replacement policy, counting)? What do DDR5 RFM and PRAC change, and what do they
  cost?

Checks: `map_compose` matches; double-sided flips bits at ≥ 4/6 locations without TRR and beats
single-sided; TRR stops single- and double-sided completely; the flips you report agree with the
module's own count; your minimum number of sides equals sampler size + 1.

## Part 5: SECDED ECC, Hamming(39,32)

- **Exercise 5-1.** `secded_encode()`: 32 data bits plus 6 Hamming check bits plus 1 overall parity
  bit gives 39 bits (`src/part5_ecc.c` sketches one classic layout).
- **Exercise 5-2.** `secded_decode()`: compute the syndrome and the overall parity, correct single
  errors, and detect double errors. Ignore bits above 38.
- **Discussion 5-1.** The harness hammers ECC-protected words in the module and prints how many
  had 0/1/2/3+ bit errors and what your decoder did. Why are multi-bit errors per word rare here,
  and what is known about real chips (ECCploit, Cojocar et al. 2019)? What fraction of *triple*
  errors were silently mis-corrected in the unit test, and why?
- **Discussion 5-2 (cost).** Compare the storage overhead of 39/32 with 72/64 and with Chipkill
  or symbol-based codes. Why is ECC not a Rowhammer fix? (Think about multi-bit flips and about
  the timing of corrections.)

Checks: round trip; all 39 single-bit errors corrected; all 741 double-bit errors detected; high
bits ignored; correct behaviour on the hammered words.

## Part 6: mitigations in the memory controller (simulator)

The controller calls your `*_on_activate(state, sim, bank, row)` for every activation and
`graphene_on_window()` once per `tREFW`. Use `dsim_mc_refresh(sim, bank, r)` to refresh row `r`;
each refresh keeps the bank busy for `tRC`. The harness runs an attack suite (double-sided,
8-sided, single-sided and a distance-2 "far" pattern) and a benign workload (random lines plus
streaming) with no mitigation, with your PARA, and with your Graphene. TRR is off.

- **Exercise 6-1.** PARA: `para_probability()` derives `p` from the datasheet (`mac`) for a target
  failure probability; `para_on_activate()` refreshes neighbours with probability `p`. Remember
  the blast radius.
- **Exercise 6-2.** Graphene: `graphene_params()` chooses the threshold `T` and the table size `N`
  from `mac`, `w2_max` and `max_act_per_window`; `graphene_on_activate()` performs the Misra–Gries
  update with a spillover counter and refreshes the neighbours whenever a count reaches a multiple
  of `T`; `graphene_on_window()` resets the table.
- **Discussion 6-1.** Show your derivation of `p` and the overhead you measured. What happens if
  you refresh only distance-1 neighbours and the distance-2 weight grows (as it does in newer
  chips)?
- **Discussion 6-2.** Derive `T` and `N`. Where does the factor 2 for table resets that are not
  aligned with the victim's refresh come from? How do your own refreshes interact with Half-Double?
  Compare PARA and Graphene: guarantee, refresh overhead, and hardware cost (entries per bank, CAM
  vs SRAM).

Checks: the suite flips bits without a mitigation; PARA (with `0 < p ≤ 0.05`) and Graphene both
reduce it to zero flips; Graphene costs ≤ 1 refresh per 1000 activations on the benign workload
and ≤ 20 per 1000 under attack.

---------------------------------------------------------------------------------------------------

## Hints

- Print. The harnesses accept `-v`, and your functions may `printf` freely, which helps in Part 2.
  `--reveal` shows the answer *after* a run when you are stuck debugging, but don't let it do the
  work for you.
- Part 2: the median of a dozen alternating accesses is very robust. Re-measure only when the
  value is close to the threshold.
- Part 3: GF(2) means XOR is addition and AND is multiplication. Keep difference vectors as
  `uint64_t` bit masks, reduce them to row-echelon form incrementally, and stop once the rank is
  `#varying bits − #functions`. If the rank goes higher, a false positive slipped in, so start over.
- Part 4: a hammer that runs for exactly one window can miss half of it, because the victim's
  refresh lands somewhere in the middle. Test both data polarities: true and anti cells live in
  different blocks of rows.
- Part 6: misaligned resets and two-sided disturbance each cost a factor of 2, and distance-2
  neighbours add `2·w2`. Size Graphene so that the spillover counter can never reach `T`.

## Going further

- Feed your Part 3 functions (plus a pivot per function) into Part 4 instead of the true map.
- Implement a Blacksmith-style non-uniform pattern generator (frequency, phase, amplitude) and
  compare it against the TRR model. Then make the sampler smarter (e.g. Space-Saving counts that
  inherit the evicted count) and see which patterns survive.
- Half-Double: hammer a far aggressor at distance 2 under Graphene with distance-1 refreshes only,
  and let the mitigation's own refreshes become the near aggressors.
- Model an ECC DIMM: store 72-bit words, correct on every read, and count flips per word as the
  hammering time grows (ECCploit-style timing of corrections).
- Measure row-buffer conflicts natively with `common/shd.h` (timed accesses with flushes) on a bare-metal
  x86_64 Linux box. This is timing only: **do not write hammering loops for real hardware.**

## Files

```
include/lab4.h           prototypes of everything you implement (read first)
src/partN_*.c            YOUR code (starter with TODOs)
solutions/partN_*.c      reference solutions (spoilers)
dramsim/dramsim.{h,c}    the DRAM model; dramsim_oracle.h = ground truth for the harness only
support/pagemap.{h,c}    native + simulated /proc/self/pagemap backends
harness/                 one driver per part (+ hcommon.c); they print results and RESULT lines
check.py                 grader used by make check / make check-solution
report.md                template for the Discussion answers
```

## References

- Y. Kim et al., "Flipping Bits in Memory Without Accessing Them: An Experimental Study of DRAM
  Disturbance Errors," ISCA 2014. (Rowhammer; PARA)
- M. Seaborn and T. Dullien, "Exploiting the DRAM rowhammer bug to gain kernel privileges,"
  Google Project Zero / Black Hat 2015. (Why pagemap PFNs became privileged.)
- P. Pessl, D. Gruss, C. Maurice, M. Schwarz, S. Mangard, "DRAMA: Exploiting DRAM Addressing for
  Cross-CPU Attacks," USENIX Security 2016. (Bank functions from row-buffer timing.)
- P. Frigo et al., "TRRespass: Exploiting the Many Sides of Target Row Refresh," IEEE S&P 2020.
- H. Hassan et al., "Uncovering In-DRAM RowHammer Protection Mechanisms: A New Methodology, Custom
  RowHammer Patterns, and Implications," MICRO 2021. (U-TRR)
- P. Jattke et al., "Blacksmith: Scalable Rowhammering in the Frequency Domain," IEEE S&P 2022.
- A. Kogler et al., "Half-Double: Hammering From the Next Row Over," USENIX Security 2022.
- Y. Park et al., "Graphene: Strong yet Lightweight Row Hammer Protection," MICRO 2020.
- J. Misra and D. Gries, "Finding repeated elements," Science of Computer Programming, 1982.
- L. Cojocar et al., "Exploiting Correcting Codes: On the Effectiveness of ECC Memory Against
  Rowhammer Attacks," IEEE S&P 2019. (ECCploit)
- J. S. Kim et al., "Revisiting RowHammer: An Experimental Analysis of Modern DRAM Devices and
  Mitigation Techniques," ISCA 2020.
- O. Mutlu and J. S. Kim, "RowHammer: A Retrospective," IEEE TCAD 2019.
- R. W. Hamming, "Error Detecting and Error Correcting Codes," Bell System Technical Journal, 1950.
- Linux kernel documentation, `Documentation/admin-guide/mm/pagemap.rst`.
