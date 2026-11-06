# Lab 4 report: From virtual addresses to DRAM disturbance

Name:
Date:
Machine / container (arch, kernel, native or sim pagemap):
Seed(s) used for the numbers below (`SEED=... make check`):

Keep each answer short and back it with numbers or output from the harness programs.

## Part 1: virtual ↔ physical

### Discussion 1-1: Why hide PFNs, and how do attackers cope without them?

### Discussion 1-2: What auto mode did on your machine; huge-page contiguity

## Part 2: bank conflicts by timing

### Discussion 2-1: Histogram, threshold, noise sources, accesses per decision

### Discussion 2-2: Defense: page policies and other ways to observe conflicts

## Part 3: bank function recovery

### Discussion 3-1: Your functions (DRAMA notation), their family, and the bits you cannot see

### Discussion 3-2: Measurement cost, brute force vs linear algebra, false positives

## Part 4: hammering

### Discussion 4-1: Success table: single vs double vs many-sided, data pattern, 1→0 vs 0→1

### Discussion 4-2: What the minimum number of sides reveals about TRR; many-sided vs non-uniform patterns

### Discussion 4-3: Defense: a sampler that resists your pattern; DDR5 RFM / PRAC

## Part 5: SECDED ECC

### Discussion 5-1: Hammered words by number of bit errors; real multi-bit flips; triple-error mis-correction

### Discussion 5-2: Cost: 39/32 vs 72/64 vs Chipkill; why ECC is not a Rowhammer fix

## Part 6: memory-controller mitigations

### Discussion 6-1: PARA: derivation of p, measured overhead, blast radius

### Discussion 6-2: Graphene: derivation of T and N, reset misalignment, Half-Double, PARA vs Graphene
