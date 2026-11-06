/* part4_main.c - harness for Part 4 (hammering in the simulator). Provided; do not edit.
 *
 *   ./build/part4 [--seed N] [-v] [--reveal]
 * The harness gives your code the TRUE mapping (dram_map_t) so that Part 4 does not depend on
 * Part 3. Two module instances are used: one without and one with the in-DRAM TRR sampler.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcommon.h"

typedef struct { hammer_kind_t kind; int sides; const char *name; } kind_t;

typedef struct {
    int locs_flipped, victims, victims_flipped, flips, f10, f01;
} agg_t;

int main(int argc, char **argv) {
    hargs_t a;
    hargs_parse(argc, argv, &a, NULL);
    hres_t r = {4, 0, 0};
    uint64_t rs = a.seed ^ 0x4444;
    dsim_t *sim[2] = {hsim(a.seed, 0), hsim(a.seed, 1)};
    const dsim_info_t *in = dsim_info(sim[0]);
    dsim_truth_t t;
    dsim_oracle_truth(sim[0], &t);
    dram_map_t m;
    hmap_from_oracle(sim[0], &m);
    printf("== Part 4: hammering  (tREFW %.1f ms, MAC %llu, rows %u..%u of each bank are yours)\n", in->tREFW_ns / 1e6,
           (unsigned long long)in->mac, m.row_first, m.row_first + m.row_count - 1);

    /* ---- Exercise 4-1: compose ---- */
    {
        int good = 0, n = 2000;
        for (int i = 0; i < n; i++) {
            unsigned bank = (unsigned)(hrand(&rs) % in->nbanks);
            unsigned row = m.row_first + (unsigned)(hrand(&rs) % m.row_count);
            unsigned col = (unsigned)(hrand(&rs) % in->row_bytes);
            uint64_t want = dsim_oracle_compose(sim[0], bank, row, col), got = map_compose(&m, bank, row, col);
            good += (want == got);
            if (want != got && a.verbose)
                printf("    bank %u row %u col %u: expected %llx got %llx\n", bank, row, col, (unsigned long long)want,
                       (unsigned long long)got);
        }
        hcheck(&r, good == n, "map_compose", "%d/%d", good, n);
    }

    /* ---- Exercise 4-2: the success table ---- */
    const kind_t kinds[] = {{HAMMER_SINGLE, 1, "single-sided"}, {HAMMER_DOUBLE, 2, "double-sided"},
                            {HAMMER_MANY, 4, "4-sided"}, {HAMMER_MANY, 8, "8-sided"}};
    enum { NK = 4, NL = 6 };
    agg_t tab[2][NK];
    memset(tab, 0, sizeof tab);
    unsigned lb[NL], lr[NL];
    for (int l = 0; l < NL; l++) {
        lb[l] = (unsigned)(hrand(&rs) % in->nbanks);
        lr[l] = m.row_first + 4 + (unsigned)(hrand(&rs) % (m.row_count - 80));
    }
    long long sim_flips[2] = {0, 0}, user_flips[2] = {0, 0};
    double dur = 2.0 * in->tREFW_ns;
    for (int trr = 0; trr < 2; trr++) {
        for (int k = 0; k < NK; k++) {
            for (int l = 0; l < NL; l++) {
                int any = 0;
                for (int p = 0; p < 2; p++) {
                    hammer_cfg_t cfg = {kinds[k].kind, kinds[k].sides, lb[l], lr[l], p ? 0ull : ~0ull, dur};
                    hammer_result_t res;
                    memset(&res, 0, sizeof res);
                    dsim_stats_t s0, s1;
                    dsim_get_stats(sim[trr], &s0);
                    int rc = hammer_once(sim[trr], &m, &cfg, &res);
                    dsim_get_stats(sim[trr], &s1);
                    if (rc != 0) memset(&res, 0, sizeof res);
                    agg_t *g = &tab[trr][k];
                    g->victims += res.victims;
                    g->victims_flipped += res.victims_flipped;
                    g->flips += res.flips;
                    g->f10 += res.flips_1to0;
                    g->f01 += res.flips_0to1;
                    any |= res.flips > 0;
                    sim_flips[trr] += (long long)(s1.flips - s0.flips);
                    user_flips[trr] += res.flips;
                }
                tab[trr][k].locs_flipped += any;
            }
        }
    }
    printf("\n  %-13s | %-36s | %-36s\n", "pattern", "TRR off", "TRR on");
    printf("  %-13s | %-36s | %-36s\n", "", "locs  victim-rows  flips (1->0/0->1)", "locs  victim-rows  flips (1->0/0->1)");
    for (int k = 0; k < NK; k++) {
        printf("  %-13s", kinds[k].name);
        for (int trr = 0; trr < 2; trr++) {
            agg_t *g = &tab[trr][k];
            char cell[96];
            snprintf(cell, sizeof cell, "%d/%d  %4d/%-4d   %5d (%d/%d)", g->locs_flipped, NL, g->victims_flipped,
                     g->victims, g->flips, g->f10, g->f01);
            printf(" | %-36s", cell);
        }
        printf("\n");
    }
    printf("  (%d locations x 2 data patterns, %.0f ms of hammering each)\n\n", NL, dur / 1e6);
    hcheck(&r, tab[0][1].locs_flipped >= NL - 2, "double-sided flips bits without TRR", "%d/%d locations",
           tab[0][1].locs_flipped, NL);
    hcheck(&r, tab[0][1].flips >= tab[0][0].flips && tab[0][1].flips > 0, "double-sided beats single-sided",
           "%d vs %d flips", tab[0][1].flips, tab[0][0].flips);
    hcheck(&r, tab[1][0].flips == 0 && tab[1][1].flips == 0 && tab[1][0].victims > 0 && tab[1][1].victims > 0,
           "TRR stops single- and double-sided", "%d and %d flips", tab[1][0].flips, tab[1][1].flips);
    int honest = sim_flips[0] > 0;
    for (int trr = 0; trr < 2; trr++)
        if (user_flips[trr] > sim_flips[trr] || (double)user_flips[trr] < 0.4 * (double)sim_flips[trr]) honest = 0;
    hcheck(&r, honest, "reported flips match the module", "you %lld/%lld, module %lld/%lld (TRR off/on)", user_flips[0],
           user_flips[1], sim_flips[0], sim_flips[1]);

    /* ---- Exercise 4-3: how big is the TRR sampler? ---- */
    dsim_t *fresh = hsim(a.seed, 1);
    dsim_stats_t s0, s1;
    dsim_get_stats(fresh, &s0);
    int ns = min_sides_under_trr(fresh, &m, a.seed);
    dsim_get_stats(fresh, &s1);
    hcheck(&r, ns == t.trr_entries + 1, "min_sides_under_trr", "%d (%.1f s of simulated hammering)", ns,
           dsim_now_ns(fresh) / 1e9);
    if (ns == t.trr_entries + 1)
        printf("  -> the sampler tracks at most %d rows per bank; %d aggressors overflow it\n", ns - 1, ns);
    if (a.reveal)
        printf("  [reveal] TRR entries %d, acts every %d REF, threshold %d; w2 %.2f; f_same %.2f; hc median %.0f\n",
               t.trr_entries, t.trr_every, t.trr_threshold, t.w2, t.f_same, t.hc_median);
    (void)s0; (void)s1;
    dsim_destroy(fresh);
    dsim_destroy(sim[0]);
    dsim_destroy(sim[1]);
    return hfinish(&r);
}
