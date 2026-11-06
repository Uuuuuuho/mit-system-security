/* part4_hammer.c - Part 4: hammering in the simulator.                (YOUR CODE GOES HERE)
 *
 * Exercise 4-1  map_compose
 * Exercise 4-2  hammer_once           (single-, double- and many-sided)
 * Exercise 4-3  min_sides_under_trr   (TRRespass-style: how many aggressors beat TRR?)
 *
 * Build and test:  make && ./build/part4 --seed 1
 * Everything here runs inside dramsim. There is no code in this lab that hammers real memory.
 */
#include <stdlib.h>
#include <string.h>

#include "../include/lab4.h"

#define MAX_SIDES 32

static uint64_t next_rand(uint64_t *st) { /* splitmix64 */
    uint64_t z = (*st += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* ---------------------------------------------------------------- Exercise 4-1 */

uint64_t map_compose(const dram_map_t *m, unsigned bank, unsigned row, unsigned col) {
    /* TODO: (1) put `row` into the row bits (from m->row_lo up), (2) scatter the 13 bits of `col`
     * into the positions of m->col_mask (lowest first), (3) for every function i whose parity
     * does not equal bit i of `bank`, flip its pivot bit m->pivot[i]. */
    (void)m; (void)bank; (void)row; (void)col;
    return 0;
}

/* ---------------------------------------------------------------- Exercise 4-2 */

int hammer_once(dsim_t *sim, const dram_map_t *m, const hammer_cfg_t *cfg, hammer_result_t *res) {
    memset(res, 0, sizeof *res);
    /* TODO:
     *  1. work out the aggressor rows and the victim rows for cfg->kind (see lab4.h); for
     *     HAMMER_SINGLE add a dummy row far away (>= 16 rows) in the same bank, so that every
     *     access to the aggressor is a row conflict; check that all rows lie inside
     *     [m->row_first, m->row_first + m->row_count)
     *  2. write cfg->pattern to every 64-bit word of the victims and ~cfg->pattern to the
     *     aggressors (dsim_write64 + map_compose)
     *  3. hammer: access one address of each aggressor round-robin until cfg->duration_ns of
     *     simulated time has passed (dsim_now_ns)
     *  4. read the victims back; count flipped bits, 1->0 and 0->1 separately, and victims with
     *     at least one flip */
    (void)sim; (void)m; (void)cfg; (void)next_rand;
    return -1;
}

/* ---------------------------------------------------------------- Exercise 4-3 */

int min_sides_under_trr(dsim_t *sim, const dram_map_t *m, uint64_t seed) {
    /* TODO: with TRR enabled, sweep the number of aggressors n = 2, 3, ... and try many-sided
     * patterns at several random locations (both data polarities!). Return the first n that
     * produces flips. What does that number tell you about the TRR sampler? */
    uint64_t st = seed;
    (void)sim; (void)m; (void)st;
    return -1;
}
