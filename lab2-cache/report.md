# lab2-cache report

Name:
Machine(s) used (sim / native: CPU model, OS, VM or bare metal, SMT on/off):

Answer each Discussion item briefly. Use tables, plots and short code excerpts where they help. When
you quote a measurement, give the command (including `--seed`) so it can be reproduced.

## Part 1: latency histograms and thresholds

### Discussion 1-3: geometry and address bits

| | sim L1 | sim L2 | sim LLC | my L1 | my L2 | my LLC |
|---|---|---|---|---|---|---|
| size | | | | | | |
| ways | | | | | | |
| sets | | | | | | |
| offset bits | | | | | | |
| index bits | | | | | | |
| index bits hidden by a 4 KiB page | | | | | | |
| lines needed to evict one line | | | | | | |

How did you choose the eviction lines for L1 and for L2?

### Discussion 1-4: histograms

(histograms or plot; overlaps; tails; effect of `SHD_SIM_NOISE=3`; native comparison)

## Part 2: Flush+Reload

### Discussion 2-2: reload order and the prefetcher

(results with sequential order, with `SHD_SIM_PREFETCH=0`, explanation; why 128-byte slots in sim
and page-sized slots natively)

### Discussion 2-3: what Flush+Reload needs, and defenses

| Requirement | Defense that removes it | Cost |
|---|---|---|
| | | |

Why partitioning does not help; how to fix the victim.

## Part 3: Prime+Probe

### Discussion 3-4: prime order, cascades and the replacement policy

(miss histograms for the victim set vs a quiet set; single vs forward+backward prime; PLRU vs LRU;
your detection rule)

### Discussion 3-5: without huge pages

(which set-index bits are learnable; eviction-set construction by timing)

## Part 4: key from code paths

### Discussion 4-4: finding the real table set

(look-alike candidates, stuck lines, what your score measures)

### Discussion 4-5: cost and limits of the constant-trace fix

(cost here; cost for AES T-tables; CacheBleed; alternatives)

## Part 5: covert channel

### Discussion 5-2: protocol and performance

(protocol description; table of slot length vs bit rate vs frame error rate; bits/s at 3 GHz)

| slot (ticks) | payload bits / Mtick | frame error rate | bits/s at 3 GHz |
|---|---|---|---|
| | | | |

### Discussion 5-3: partitioning and other defenses

(what `SHD_SIM_PARTITION=1` does to your channel and why; cost of partitioning; what remains open;
comparison with randomized caches, SMT off, noise)
