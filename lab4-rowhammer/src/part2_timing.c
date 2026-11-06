/* part2_timing.c - Part 2: bank conflicts by timing.                  (YOUR CODE GOES HERE)
 *
 * Exercise 2-1  measure_pair
 * Exercise 2-2  calibrate_threshold
 * Exercise 2-3  same_bank_diff_row
 *
 * Build and test:  make && ./build/part2 --seed 1
 * You may only use the public API in dramsim/dramsim.h (dsim_access, dsim_info, ...).
 */
#include <math.h>
#include <stdlib.h>

#include "../include/lab4.h"

/* ---- helpers you may use ---- */
static int cmp_u32(const void *x, const void *y) {
    uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    return (a > b) - (a < b);
}
static uint64_t next_rand(uint64_t *st) { /* splitmix64 */
    uint64_t z = (*st += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
/* a random cache-line-aligned physical address inside your buffer */
static uint64_t rand_line(const dsim_info_t *in, uint64_t *st) {
    return in->buf_base + (next_rand(st) % in->buf_size & ~63ull);
}

/* ---------------------------------------------------------------- Exercise 2-1 */

double measure_pair(dsim_t *sim, uint64_t a, uint64_t b, int rounds) {
    /* TODO: access a, b, a, b, ... (`rounds` times each) with dsim_access() and return a robust
     * summary of the latencies. Think about: the very first access of each address, refresh
     * stalls, interrupt-like outliers, and other cores opening rows behind your back.
     * (qsort with cmp_u32 gives you a median.) */
    (void)sim; (void)a; (void)b; (void)rounds; (void)cmp_u32;
    return 0.0;
}

/* ---------------------------------------------------------------- Exercise 2-2 */

double calibrate_threshold(dsim_t *sim, uint64_t seed) {
    /* TODO: measure many random pairs (a fixed base against random lines), look at the
     * distribution (print a histogram!), and place a threshold between the two populations.
     * Same-bank-different-row pairs are rare (about 1 in nbanks). */
    uint64_t st = seed;
    (void)sim; (void)st; (void)rand_line;
    return 0.0;
}

/* ---------------------------------------------------------------- Exercise 2-3 */

int same_bank_diff_row(dsim_t *sim, uint64_t a, uint64_t b, double threshold) {
    /* TODO: 1 if (a, b) is a row-buffer conflict pair. Parts 3 and 4 build on this, so it has to
     * be reliable (>= 99% accuracy): spend a few more accesses when the answer is close. */
    (void)sim; (void)a; (void)b; (void)threshold;
    return 0;
}
