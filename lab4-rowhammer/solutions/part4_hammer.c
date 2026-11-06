/* part4_hammer.c - REFERENCE SOLUTION for Part 4. SPOILER: try the lab yourself first.
 *
 * Exercise 4-1  map_compose          row bits, column bits, then fix each bank bit with its pivot
 * Exercise 4-2  hammer_once          fill victims/aggressors, hammer round-robin, diff the victims
 * Exercise 4-3  min_sides_under_trr  sweep the number of aggressors until TRR stops working
 */
#include <stdlib.h>
#include <string.h>

#include "../include/lab4.h"

#define MAX_SIDES 32

static uint64_t next_rand(uint64_t *st) {
    uint64_t z = (*st += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

uint64_t map_compose(const dram_map_t *m, unsigned bank, unsigned row, unsigned col) {
    uint64_t pa = (uint64_t)row << m->row_lo;
    /* deposit the column bits into the column positions, lowest first */
    int j = 0;
    for (int b = 0; b < m->row_lo; b++)
        if ((m->col_mask >> b) & 1) { if ((col >> j) & 1) pa |= 1ull << b; j++; }
    /* every function owns a pivot bit: flip it when the bank bit comes out wrong */
    for (int i = 0; i < m->nfns; i++)
        if (__builtin_parityll(pa & m->fn[i]) != (int)((bank >> i) & 1)) pa ^= 1ull << m->pivot[i];
    return pa;
}

static void fill_row(dsim_t *sim, const dram_map_t *m, unsigned bank, unsigned row, uint64_t v) {
    unsigned rb = dsim_info(sim)->row_bytes;
    for (unsigned c = 0; c < rb; c += 8) dsim_write64(sim, map_compose(m, bank, row, c), v);
}

int hammer_once(dsim_t *sim, const dram_map_t *m, const hammer_cfg_t *cfg, hammer_result_t *res) {
    const dsim_info_t *in = dsim_info(sim);
    unsigned aggr[MAX_SIDES + 1], vict[MAX_SIDES + 2];
    int na = 0, nv = 0, nh;
    memset(res, 0, sizeof *res);
    if (cfg->bank >= in->nbanks) return -1;
    unsigned lo = m->row_first, hi = m->row_first + m->row_count - 1, r = cfg->row;

    switch (cfg->kind) {
    case HAMMER_SINGLE:
        aggr[na++] = r;
        vict[nv++] = r - 1;
        vict[nv++] = r + 1;
        /* dummy: far away in the same bank, only there to force a row conflict every time */
        aggr[na] = (r + 40 + 2 <= hi) ? r + 40 : r - 40;
        nh = na + 1;
        break;
    case HAMMER_DOUBLE:
    case HAMMER_MANY: {
        int n = cfg->kind == HAMMER_DOUBLE ? 2 : cfg->sides;
        if (n < 2 || n > MAX_SIDES) return -1;
        for (int i = 0; i < n; i++) aggr[na++] = r + 2u * (unsigned)i;
        for (int i = -1; i <= 2 * n - 1; i += 2) vict[nv++] = (unsigned)((int)r + i);
        nh = na;
        break;
    }
    default:
        return -1;
    }
    for (int i = 0; i < nh; i++) if (aggr[i] < lo || aggr[i] > hi) return -1;
    for (int i = 0; i < nv; i++) if (vict[i] < lo || vict[i] > hi) return -1;

    /* data pattern: victims hold `pattern`, aggressors the complement (worst case for the victims) */
    for (int i = 0; i < nv; i++) fill_row(sim, m, cfg->bank, vict[i], cfg->pattern);
    for (int i = 0; i < nh; i++) fill_row(sim, m, cfg->bank, aggr[i], ~cfg->pattern);

    /* hammer: every access is a row conflict, i.e. one activation of an aggressor */
    uint64_t pa[MAX_SIDES + 1];
    for (int i = 0; i < nh; i++) pa[i] = map_compose(m, cfg->bank, aggr[i], 0);
    double end = dsim_now_ns(sim) + cfg->duration_ns;
    while (dsim_now_ns(sim) < end)
        for (int i = 0; i < nh; i++) dsim_access(sim, pa[i]);

    /* check */
    res->victims = nv;
    for (int i = 0; i < nv; i++) {
        int f = 0;
        for (unsigned c = 0; c < in->row_bytes; c += 8) {
            uint64_t got = dsim_read64(sim, map_compose(m, cfg->bank, vict[i], c));
            uint64_t diff = got ^ cfg->pattern;
            f += __builtin_popcountll(diff);
            res->flips_1to0 += __builtin_popcountll(diff & cfg->pattern);
            res->flips_0to1 += __builtin_popcountll(diff & ~cfg->pattern);
        }
        res->flips += f;
        res->victims_flipped += f > 0;
    }
    return 0;
}

int min_sides_under_trr(dsim_t *sim, const dram_map_t *m, uint64_t seed) {
    const dsim_info_t *in = dsim_info(sim);
    uint64_t st = seed ^ 0x7A11;
    for (int n = 2; n <= 24; n++) {
        for (int l = 0; l < 12; l++) {
            unsigned bank = (unsigned)(next_rand(&st) % in->nbanks);
            unsigned row = m->row_first + 2 + (unsigned)(next_rand(&st) % (m->row_count - 2u * (unsigned)n - 8));
            for (int p = 0; p < 2; p++) { /* both polarities: true cells and anti cells */
                hammer_cfg_t cfg = {HAMMER_MANY, n, bank, row, p ? 0ull : ~0ull, 2.0 * in->tREFW_ns};
                hammer_result_t res;
                if (hammer_once(sim, m, &cfg, &res) == 0 && res.flips > 0) return n;
            }
        }
    }
    return -1;
}
