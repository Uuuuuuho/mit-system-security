/* part6_mitigate.c - Part 6: Rowhammer mitigations in the memory controller. (YOUR CODE GOES HERE)
 *
 * Exercise 6-1  PARA      probabilistic adjacent-row activation
 * Exercise 6-2  Graphene  Misra-Gries heavy-hitter tracking
 *
 * Build and test:  make && ./build/part6 --seed 1
 * The controller calls *_on_activate(state, sim, bank, row) for every row activation; call
 * dsim_mc_refresh(sim, bank, r) to refresh row r of that bank (rows r-1 / r+1 are the physical
 * neighbours; out-of-range rows are ignored). graphene_on_window is called once per tREFW.
 * Use the datasheet facts in dsim_info_t: mac, blast_radius, w2_max, max_act_per_window.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../include/lab4.h"

/* ---------------------------------------------------------------- Exercise 6-1: PARA */

typedef struct {
    double p;
    uint64_t rng;
    /* TODO: anything else you need */
} para_t;

double para_probability(const dsim_info_t *info) {
    /* TODO: pick p so that a victim practically never collects `info->mac` disturbance without
     * being refreshed. Write down your failure-probability argument in report.md. */
    (void)info;
    return 0.0;
}

void *para_create(const dsim_info_t *info, double p, uint64_t seed) {
    para_t *s = (para_t *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->p = p;
    s->rng = seed;
    (void)info;
    return s;
}

void para_on_activate(void *state, dsim_t *sim, unsigned bank, unsigned row) {
    /* TODO: with probability p, refresh the neighbours of `row`. Which neighbours? (blast radius) */
    (void)state; (void)sim; (void)bank; (void)row;
}

void para_destroy(void *state) { free(state); }

/* ---------------------------------------------------------------- Exercise 6-2: Graphene */

typedef struct {
    unsigned T, N, nbanks;
    /* TODO: per-bank table of N (row, count) entries and a spillover counter per bank */
} graphene_t;

graphene_params_t graphene_params(const dsim_info_t *info) {
    /* TODO: choose the threshold T (how many activations of one row may happen before you
     * refresh its neighbours?) and the number of entries N (Misra-Gries: the spillover counter
     * stays below W/(N+1), and it must stay below T). Justify both in report.md. */
    graphene_params_t p = {0, 0};
    (void)info;
    return p;
}

void *graphene_create(const dsim_info_t *info, graphene_params_t p) {
    graphene_t *g = (graphene_t *)calloc(1, sizeof *g);
    if (!g) return NULL;
    g->T = p.threshold;
    g->N = p.entries;
    g->nbanks = info->nbanks;
    /* TODO: allocate the tables */
    return g;
}

void graphene_on_window(void *state, dsim_t *sim) {
    /* TODO: reset all tables (the guarantee is per refresh window) */
    (void)state; (void)sim;
}

void graphene_on_activate(void *state, dsim_t *sim, unsigned bank, unsigned row) {
    /* TODO: Misra-Gries update for `row` in `bank`:
     *   tracked            -> count++
     *   untracked, some entry has count == spill -> replace it, count = spill + 1
     *   otherwise          -> spill++
     * and whenever a count reaches a multiple of T, refresh the neighbours. */
    (void)state; (void)sim; (void)bank; (void)row;
}

void graphene_destroy(void *state) {
    /* TODO: free the tables too */
    free(state);
}
