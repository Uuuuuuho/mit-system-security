/* part6_mitigate.c - REFERENCE SOLUTION for Part 6. SPOILER: try the lab yourself first.
 *
 * Exercise 6-1  PARA     (Kim et al., ISCA 2014): on every ACT, with probability p, refresh all
 *                        rows within the blast radius of the activated row.
 * Exercise 6-2  Graphene (Park et al., MICRO 2020): per-bank Misra-Gries table with a spillover
 *                        counter; refresh the neighbours whenever an estimate hits k*T; reset the
 *                        table every refresh window.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../include/lab4.h"

/* ------------------------------------------------------------------ PARA */
typedef struct {
    double p;
    uint64_t rng;
    int radius;
} para_t;

static uint64_t next_rand(uint64_t *st) {
    uint64_t z = (*st += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double para_probability(const dsim_info_t *info) {
    /* A victim needs at least MAC disturbing activations of its neighbours (each counts <= 1).
     * Every one of them gives PARA an independent chance p to refresh the victim, so the chance
     * to reach MAC unrefreshed is (1-p)^MAC ~ exp(-p * MAC). Aim for 1e-9 per attempt. */
    return log(1e9) / (double)info->mac;
}

void *para_create(const dsim_info_t *info, double p, uint64_t seed) {
    para_t *s = (para_t *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->p = p;
    s->rng = seed ^ 0xA7A;
    s->radius = info->blast_radius;
    return s;
}

void para_on_activate(void *state, dsim_t *sim, unsigned bank, unsigned row) {
    para_t *s = (para_t *)state;
    double u = (double)(next_rand(&s->rng) >> 11) * (1.0 / 9007199254740992.0);
    if (u >= s->p) return;
    for (int d = 1; d <= s->radius; d++) {
        if (row >= (unsigned)d) dsim_mc_refresh(sim, bank, row - (unsigned)d);
        dsim_mc_refresh(sim, bank, row + (unsigned)d); /* out-of-range rows are ignored */
    }
}

void para_destroy(void *state) { free(state); }

/* ------------------------------------------------------------------ Graphene */
typedef struct {
    unsigned T, N, nbanks;
    int radius;
    uint32_t *row;   /* nbanks * N */
    uint32_t *cnt;   /* nbanks * N; an empty entry has row = UINT32_MAX and count = spill */
    uint32_t *spill; /* per bank */
} graphene_t;

graphene_params_t graphene_params(const dsim_info_t *info) {
    /* Between two refreshes of a victim each of its 2 + 2 neighbours (weights 1, 1, w2, w2) can
     * be activated fewer than 2T times: < T before the table reset and < T after it (the reset
     * is not aligned with the victim's own refresh). So 2T(2 + 2*w2) must stay below MAC.
     * Keep 20 % headroom for the activations caused by our own refreshes (Half-Double). */
    graphene_params_t p;
    double T = 0.8 * (double)info->mac / (4.0 * (1.0 + info->w2_max));
    p.threshold = (unsigned)T;
    /* Misra-Gries: with N entries the spillover counter stays below W/(N+1); it must stay
     * below T so that every row that reaches T is in the table. */
    p.entries = (unsigned)(info->max_act_per_window / p.threshold) + 1;
    return p;
}

void *graphene_create(const dsim_info_t *info, graphene_params_t p) {
    graphene_t *g = (graphene_t *)calloc(1, sizeof *g);
    if (!g || p.threshold == 0 || p.entries == 0) { free(g); return NULL; }
    g->T = p.threshold;
    g->N = p.entries;
    g->nbanks = info->nbanks;
    g->radius = info->blast_radius;
    size_t n = (size_t)g->nbanks * g->N;
    g->row = (uint32_t *)malloc(n * sizeof(uint32_t));
    g->cnt = (uint32_t *)calloc(n, sizeof(uint32_t));
    g->spill = (uint32_t *)calloc(g->nbanks, sizeof(uint32_t));
    if (!g->row || !g->cnt || !g->spill) { graphene_destroy(g); return NULL; }
    memset(g->row, 0xFF, n * sizeof(uint32_t));
    return g;
}

void graphene_on_window(void *state, dsim_t *sim) {
    graphene_t *g = (graphene_t *)state;
    (void)sim;
    size_t n = (size_t)g->nbanks * g->N;
    memset(g->row, 0xFF, n * sizeof(uint32_t));
    memset(g->cnt, 0, n * sizeof(uint32_t));
    memset(g->spill, 0, g->nbanks * sizeof(uint32_t));
}

void graphene_on_activate(void *state, dsim_t *sim, unsigned bank, unsigned row) {
    graphene_t *g = (graphene_t *)state;
    uint32_t *rw = g->row + (size_t)bank * g->N, *cn = g->cnt + (size_t)bank * g->N;
    uint32_t spill = g->spill[bank];
    int hit = -1, repl = -1;
    for (unsigned i = 0; i < g->N; i++) {
        if (rw[i] == row) { hit = (int)i; break; }
        if (repl < 0 && cn[i] == spill) repl = (int)i;
    }
    if (hit < 0) {
        if (repl < 0) { g->spill[bank]++; return; } /* untracked: only the spillover grows */
        hit = repl;
        rw[hit] = row;
        cn[hit] = spill; /* incremented below to spill + 1 */
    }
    cn[hit]++;
    if (cn[hit] % g->T == 0) {
        for (int d = 1; d <= g->radius; d++) {
            if (row >= (unsigned)d) dsim_mc_refresh(sim, bank, row - (unsigned)d);
            dsim_mc_refresh(sim, bank, row + (unsigned)d);
        }
    }
}

void graphene_destroy(void *state) {
    graphene_t *g = (graphene_t *)state;
    if (!g) return;
    free(g->row);
    free(g->cnt);
    free(g->spill);
    free(g);
}
