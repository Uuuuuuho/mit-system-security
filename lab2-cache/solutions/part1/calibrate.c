/* Part 1 - REFERENCE SOLUTION (spoilers). Measure per-level latency and derive thresholds. */
#include <stdlib.h>
#include <string.h>

#include "lab.h"

/* One huge-page buffer holding 2*l2_ways lines for every L2 set. With huge pages the L2 set of an
 * address is just its VA bits 6..6+log2(l2_sets), so congruent lines are `l2_sets*line` apart. */
static uint8_t *buf;
static size_t buf_bytes, l1_stride, l2_stride;
static int nways_l2;

static void setup(void) {
    if (buf) return;
    const plat_geom_t *g = plat_geom();
    l1_stride = (size_t)g->l1_sets * (size_t)g->line;
    l2_stride = (size_t)g->l2_sets * (size_t)g->line;
    nways_l2 = 2 * g->l2_ways;
    buf_bytes = l2_stride * (size_t)nways_l2;
    if (buf_bytes < g->huge) buf_bytes = g->huge;
    buf = plat_alloc(buf_bytes, 1);
}

static uint8_t *random_line(void) {
    const plat_geom_t *g = plat_geom();
    return buf + (plat_rand() % (uint64_t)g->l2_sets) * (size_t)g->line; /* first "way" region */
}

/* Kick x out of L1 only: touch 2*l1_ways lines that share x's L1 set but not its L2 set. */
static void evict_l1(uint8_t *x) {
    const plat_geom_t *g = plat_geom();
    size_t ratio = l2_stride / l1_stride; /* how many L1 strides make one L2 stride */
    int done = 0;
    for (size_t j = 1; done < 2 * g->l1_ways; j++) {
        if (ratio > 1 && j % ratio == 0) continue; /* would be L2-congruent with x */
        uint8_t *y = x + j * l1_stride;
        if (y >= buf + buf_bytes) y -= buf_bytes;
        plat_access(y);
        done++;
    }
}

/* Kick x out of L1 and L2 (but not the LLC): touch every other L2-congruent line, twice
 * (tree-PLRU needs more than `ways` misses to be sure). */
static void evict_l2(uint8_t *x) {
    for (int pass = 0; pass < 2; pass++)
        for (int j = 1; j < nways_l2; j++) plat_access(x + (size_t)j * l2_stride);
}

void measure_level(int level, uint64_t *samples, int n) {
    setup();
    for (int k = 0; k < n; k++) {
        uint8_t *x = random_line();
        switch (level) {
        case LVL_L1:
            plat_access(x);
            break;
        case LVL_L2:
            plat_access(x);
            evict_l1(x);
            break;
        case LVL_LLC:
            plat_access(x);
            evict_l2(x);
            break;
        default: /* DRAM */
            plat_access(x);
            plat_flush(x);
            break;
        }
        plat_fence();
        samples[k] = plat_probe(x);
    }
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Threshold t minimising  #(fast > t) + #(slow <= t), searched between the two medians.
 * Among equally good thresholds take the one in the middle (largest safety margin). */
static uint64_t best_split(const uint64_t *fast, int nf, const uint64_t *slow, int ns) {
    uint64_t lo = fast[nf / 2], hi = slow[ns / 2];
    if (hi <= lo) return lo;
    uint64_t step = (hi - lo) / 2048 + 1;
    long best_err = -1;
    uint64_t first = lo, last = lo;
    for (uint64_t t = lo; t <= hi; t += step) {
        long err = 0;
        for (int i = 0; i < nf; i++) err += fast[i] > t;
        for (int i = 0; i < ns; i++) err += slow[i] <= t;
        if (best_err < 0 || err < best_err) { best_err = err; first = last = t; }
        else if (err == best_err) last = t;
    }
    return first + (last - first) / 2;
}

cal_t calibrate(void) {
    enum { N = 1000 };
    static uint64_t s[5][N];
    cal_t c;
    memset(&c, 0, sizeof c);
    for (int l = 1; l <= 4; l++) {
        measure_level(l, s[l], N);
        qsort(s[l], N, sizeof(uint64_t), cmp_u64);
        c.med[l] = s[l][N / 2];
    }
    /* "hit in L1 or L2" vs "LLC": use the L2 samples as the fast class (L1 is even faster) */
    c.l2_miss = best_split(s[LVL_L2], N, s[LVL_LLC], N);
    c.llc_miss = best_split(s[LVL_LLC], N, s[LVL_DRAM], N);
    return c;
}
