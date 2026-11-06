/* part2_timing.c - REFERENCE SOLUTION for Part 2. SPOILER: try the lab yourself first.
 *
 * Exercise 2-1  measure_pair        alternate a/b, median latency
 * Exercise 2-2  calibrate_threshold 1-D two-means over many random pairs
 * Exercise 2-3  same_bank_diff_row  threshold, with a re-measurement near the boundary
 */
#include <math.h>
#include <stdlib.h>

#include "../include/lab4.h"

static int cmp_u32(const void *x, const void *y) {
    uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    return (a > b) - (a < b);
}

static uint64_t next_rand(uint64_t *st) {
    uint64_t z = (*st += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double measure_pair(dsim_t *sim, uint64_t a, uint64_t b, int rounds) {
    enum { MAXR = 256 };
    uint32_t v[2 * MAXR];
    if (rounds < 1) rounds = 1;
    if (rounds > MAXR) rounds = MAXR;
    /* one warm-up round so that both rows have been opened at least once */
    dsim_access(sim, a);
    dsim_access(sim, b);
    for (int i = 0; i < rounds; i++) {
        v[2 * i] = dsim_access(sim, a);
        v[2 * i + 1] = dsim_access(sim, b);
    }
    /* the median ignores refresh stalls and interrupt-like spikes */
    qsort(v, (size_t)(2 * rounds), sizeof v[0], cmp_u32);
    return 0.5 * (v[rounds - 1] + v[rounds]);
}

double calibrate_threshold(dsim_t *sim, uint64_t seed) {
    const dsim_info_t *in = dsim_info(sim);
    enum { N = 1000 };
    static double v[N];
    uint64_t st = seed ^ 0xCA11B;
    uint64_t base = in->buf_base + (next_rand(&st) % in->buf_size & ~63ull);
    double lo = 1e18, hi = 0;
    for (int i = 0; i < N; i++) {
        uint64_t x = in->buf_base + (next_rand(&st) % in->buf_size & ~63ull);
        v[i] = measure_pair(sim, base, x, 8);
        if (v[i] < lo) lo = v[i];
        if (v[i] > hi) hi = v[i];
    }
    /* 1-D 2-means: the low cluster is "row hit", the high one "row conflict" */
    double c0 = lo, c1 = hi;
    for (int it = 0; it < 50; it++) {
        double s0 = 0, s1 = 0, mid = 0.5 * (c0 + c1);
        int n0 = 0, n1 = 0;
        for (int i = 0; i < N; i++) {
            if (v[i] < mid) { s0 += v[i]; n0++; }
            else { s1 += v[i]; n1++; }
        }
        if (n0) c0 = s0 / n0;
        if (n1) c1 = s1 / n1;
    }
    return 0.5 * (c0 + c1);
}

int same_bank_diff_row(dsim_t *sim, uint64_t a, uint64_t b, double threshold) {
    double m = measure_pair(sim, a, b, 6);
    if (fabs(m - threshold) < 15.0) m = measure_pair(sim, a, b, 32); /* unsure: look again */
    return m > threshold;
}
