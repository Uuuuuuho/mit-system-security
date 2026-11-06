/* part6_main.c - harness for Part 6 (mitigations in the memory controller). Provided; do not edit.
 *
 *   ./build/part6 [--seed N] [-v] [--reveal]
 * Runs a fixed attack suite (with the harness's own hammer and the true mapping) and a benign
 * workload against: no mitigation, your PARA, your Graphene. TRR is off in all runs.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcommon.h"

typedef struct {
    const char *name;
    int nag;          /* aggressor rows */
    int stride;       /* distance between aggressors */
    int dummy;        /* single-sided: add a far dummy row */
    int locs;
} pattern_t;

typedef struct {
    uint64_t flips, acts, refreshes, b_acts, b_refreshes;
    double b_time;
} outcome_t;

static const pattern_t suite[] = {
    {"double-sided", 2, 2, 0, 6},
    {"8-sided", 8, 2, 0, 2},
    {"single-sided", 1, 2, 1, 2},
    {"far (distance 2)", 2, 4, 0, 2},
};
#define NSUITE ((int)(sizeof suite / sizeof suite[0]))

static void attach(dsim_t *s, int which, void **state, const dsim_info_t *info, double p, graphene_params_t gp,
                   uint64_t seed) {
    dsim_mitigation_t m = {NULL, NULL, NULL};
    *state = NULL;
    if (which == 1) {
        *state = para_create(info, p, seed);
        m.state = *state;
        m.on_activate = para_on_activate;
    } else if (which == 2) {
        *state = graphene_create(info, gp);
        m.state = *state;
        m.on_activate = graphene_on_activate;
        m.on_window = graphene_on_window;
    }
    if (which && *state) dsim_set_mitigation(s, &m);
}

static void detach(dsim_t *s, int which, void *state) {
    dsim_set_mitigation(s, NULL);
    if (which == 1 && state) para_destroy(state);
    if (which == 2 && state) graphene_destroy(state);
}

int main(int argc, char **argv) {
    hargs_t a;
    hargs_parse(argc, argv, &a, NULL);
    hres_t r = {6, 0, 0};
    dsim_t *probe = hsim(a.seed, 0);
    dsim_info_t info = *dsim_info(probe);
    unsigned first, count;
    hbuf_rows(probe, &first, &count);
    dsim_truth_t t;
    dsim_oracle_truth(probe, &t);
    dsim_destroy(probe);
    printf("== Part 6: memory-controller mitigations  (MAC %llu, blast radius %d, w2 <= %.2f, <= %llu ACT/bank/window)\n",
           (unsigned long long)info.mac, info.blast_radius, info.w2_max, (unsigned long long)info.max_act_per_window);

    double p = para_probability(&info);
    graphene_params_t gp = graphene_params(&info);
    printf("  your PARA probability p = %.5f\n", p);
    printf("  your Graphene: threshold %u, %u entries per bank (%u banks)\n", gp.threshold, gp.entries, info.nbanks);

    /* attack locations, the same for every configuration */
    uint64_t rs = a.seed ^ 0x6666;
    unsigned lb[NSUITE][8], lr[NSUITE][8];
    for (int k = 0; k < NSUITE; k++)
        for (int l = 0; l < suite[k].locs; l++) {
            lb[k][l] = (unsigned)(hrand(&rs) % info.nbanks);
            lr[k][l] = first + 4 + (unsigned)(hrand(&rs) % (count - 80));
        }

    const char *names[3] = {"none", "PARA", "Graphene"};
    outcome_t out[3];
    memset(out, 0, sizeof out);
    for (int c = 0; c < 3; c++) {
        /* --- attack suite --- */
        dsim_t *s = hsim(a.seed, 0);
        void *st;
        attach(s, c, &st, &info, p, gp, a.seed);
        if (c && !st) printf("  (%s: create returned NULL - running without it)\n", names[c]);
        for (int k = 0; k < NSUITE; k++) {
            uint64_t f0 = 0;
            dsim_stats_t s0, s1;
            dsim_get_stats(s, &s0);
            f0 = s0.flips;
            for (int l = 0; l < suite[k].locs; l++)
                for (int pol = 0; pol < 2; pol++) {
                    unsigned ag[16];
                    int n = 0;
                    for (int i = 0; i < suite[k].nag; i++) ag[n++] = lr[k][l] + (unsigned)(i * suite[k].stride);
                    unsigned last = ag[n - 1];
                    href_fill(s, lb[k][l], lr[k][l] - 2, last + 2, ag, n, pol ? 0ull : ~0ull);
                    if (suite[k].dummy) ag[n++] = lr[k][l] + 40;
                    href_hammer(s, lb[k][l], ag, n, 2.0 * info.tREFW_ns);
                }
            dsim_get_stats(s, &s1);
            if (a.verbose) printf("    %-9s %-17s %llu flips\n", names[c], suite[k].name, (unsigned long long)(s1.flips - f0));
        }
        dsim_stats_t sa;
        dsim_get_stats(s, &sa);
        out[c].flips = sa.flips;
        out[c].acts = sa.acts;
        out[c].refreshes = sa.mc_refreshes;
        detach(s, c, st);
        dsim_destroy(s);

        /* --- benign workload: random lines mixed with streaming through 1 MiB --- */
        s = hsim(a.seed, 0);
        attach(s, c, &st, &info, p, gp, a.seed);
        uint64_t brs = a.seed ^ 0xB0B, base = info.buf_base + (hrand(&brs) % (info.buf_size - (1u << 20)) & ~63ull);
        uint64_t sp = 0;
        for (int i = 0; i < 300000; i++) {
            if (hrand(&brs) % 10 < 7) dsim_access(s, info.buf_base + (hrand(&brs) % info.buf_size & ~63ull));
            else { dsim_access(s, base + sp); sp = (sp + 64) % (1u << 20); }
        }
        dsim_get_stats(s, &sa);
        out[c].b_acts = sa.acts;
        out[c].b_refreshes = sa.mc_refreshes;
        out[c].b_time = dsim_now_ns(s);
        detach(s, c, st);
        dsim_destroy(s);
    }

    printf("\n  %-9s %8s %22s %22s %16s\n", "mitig.", "flips", "attack refresh/kACT", "benign refresh/kACT", "benign slowdown");
    for (int c = 0; c < 3; c++)
        printf("  %-9s %8llu %22.2f %22.3f %15.2f%%\n", names[c], (unsigned long long)out[c].flips,
               out[c].acts ? 1000.0 * (double)out[c].refreshes / (double)out[c].acts : 0.0,
               out[c].b_acts ? 1000.0 * (double)out[c].b_refreshes / (double)out[c].b_acts : 0.0,
               100.0 * (out[c].b_time / out[0].b_time - 1.0));
    printf("  (attack suite: ");
    for (int k = 0; k < NSUITE; k++) printf("%s%s x%d", k ? ", " : "", suite[k].name, 2 * suite[k].locs);
    printf("; %.0f ms each)\n\n", 2.0 * info.tREFW_ns / 1e6);

    hcheck(&r, out[0].flips > 0, "attack suite flips bits without a mitigation", "%llu flips", (unsigned long long)out[0].flips);
    hcheck(&r, p > 0.0 && p <= 0.05 && out[1].flips == 0, "PARA stops the suite",
           "p = %.5f, %llu flips", p, (unsigned long long)out[1].flips);
    hcheck(&r, out[2].flips == 0 && out[2].refreshes > 0, "Graphene stops the suite", "%llu flips, %llu refreshes",
           (unsigned long long)out[2].flips, (unsigned long long)out[2].refreshes);
    double g_b = out[2].b_acts ? 1000.0 * (double)out[2].b_refreshes / (double)out[2].b_acts : 1e9;
    double g_a = out[2].acts ? 1000.0 * (double)out[2].refreshes / (double)out[2].acts : 1e9;
    hcheck(&r, out[2].refreshes > 0 && g_b <= 1.0 && g_a <= 20.0, "Graphene overhead is small", "%.3f/kACT benign, %.2f/kACT under attack", g_b, g_a);
    if (a.reveal) printf("  [reveal] w2 %.2f, hc median %.0f above MAC\n", t.w2, t.hc_median);
    return hfinish(&r);
}
