/* part3_main.c - driver for Part 3 (Prime+Probe CTF). Do not edit.
 *
 *   ./bin/part3 [--seed N] [--mode easy|hard] [--lines N] [--steady]
 *
 * easy: the victim hammers 16 lines of one secret L2 set all the time.
 * hard: only 4 lines, only in bursts (active ~25% of the time, period ~100k ticks in sim), and it
 *       does more unrelated work in between.
 * --lines N / --steady override the number of victim lines / turn bursts off (for experiments).
 * The victim runs on your SMT sibling (shares L1 and L2). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "lab.h"

typedef struct {
    unsigned set;
    int nlines;
    int work;
    int bursty;
    uint64_t seed;
} pp_victim_t;

static uint64_t xs(uint64_t *s) {
    *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
    return *s;
}

/* Find `n` lines of the victim's own (small-page) memory that live in L2 set `set`.
 * The victim may use the platform's address translation: it is not the attacker. */
static int victim_lines(unsigned set, int n, uint8_t **out) {
    const plat_geom_t *g = plat_geom();
    unsigned lines_per_page = (unsigned)(g->page / (size_t)g->line);
    size_t off = (size_t)(set % lines_per_page) * (size_t)g->line;
    int k = 0;
    for (int chunk = 0; chunk < 32 && k < n; chunk++) {
        size_t len = 4u << 20;
        uint8_t *m = plat_alloc(len, 0);
        for (size_t p = 0; p < len && k < n; p += g->page) {
            uint64_t pa = hx_pa_of(m + p + off);
            if (pa == ~0ull) return -1;
            if (((pa / (uint64_t)g->line) % (uint64_t)g->l2_sets) == set) out[k++] = m + p + off;
        }
    }
    return k;
}

static void victim(void *arg) {
    pp_victim_t *v = arg;
    const plat_geom_t *g = plat_geom();
    uint8_t *lines[64];
    int k = victim_lines(v->set, v->nlines, lines);
    if (k < v->nlines) {
        /* native without pagemap access: fall back to a huge page and virtual address bits */
        fprintf(stderr, "[part3] victim: no physical addresses (need root); using huge-page VA bits\n");
        size_t stride = (size_t)g->l2_sets * (size_t)g->line;
        uint8_t *m = plat_alloc(stride * (size_t)v->nlines, 1);
        for (k = 0; k < v->nlines; k++) lines[k] = m + (size_t)k * stride + (size_t)v->set * (size_t)g->line;
    }
    size_t wlen = 2u << 20;
    uint8_t *work = plat_alloc(wlen, 0);
    uint64_t r = v->seed | 1;
    /* bursty mode: active for the first quarter of every period */
    uint64_t period = plat_is_sim() ? 100000 : 20000000, base = plat_now() - xs(&r) % period;
    for (;;) {
        int active = !v->bursty || ((plat_now() - base) % period) < period / 4;
        if (active)
            for (int i = 0; i < v->nlines; i++) plat_access(lines[i]);
        for (int i = 0; i < v->work; i++) plat_access(work + (xs(&r) % (wlen / 64)) * 64);
        plat_wait(150 + xs(&r) % 200);
    }
}

int main(int argc, char **argv) {
    hx_init(argc, argv, "part3");
    hx_set_timeout(120, (uint64_t)8e9);
    const char *mode = hx_arg("--mode");
    int hard = mode && strcmp(mode, "hard") == 0;
    cal_t cal = calibrate();
    printf("[part3] calibration: l2_miss=%llu (L2 median %llu, LLC median %llu)\n", (unsigned long long)cal.l2_miss,
           (unsigned long long)cal.med[LVL_L2], (unsigned long long)cal.med[LVL_LLC]);

    static pp_victim_t v;
    v.set = (unsigned)(hx_secret_rand() % (uint64_t)plat_geom()->l2_sets);
    v.nlines = hard ? 4 : 16;
    v.work = hard ? 3 : 1;
    v.bursty = hard && !hx_flag("--steady");
    if (hx_arg("--lines")) v.nlines = atoi(hx_arg("--lines"));
    if (v.nlines < 1 || v.nlines > 64) v.nlines = hard ? 4 : 16;
    v.seed = hx_secret_rand();
    printf("[part3] mode=%s: victim hammers %d lines of one L2 set%s\n", hard ? "hard" : "easy", v.nlines,
           v.bursty ? ", in bursts" : "");
    hx_spawn(victim, &v, HX_SIBLING, "pp-victim");

    uint64_t t0 = plat_now();
    int guess = pp_find_set(&cal, v.nlines);
    uint64_t t1 = plat_now();
    printf("[part3] attack took %.2f Mticks\n", (double)(t1 - t0) / 1e6);
    hx_result(guess == (int)v.set, "mode=%s guess=%d secret=%u", hard ? "hard" : "easy", guess, v.set);
}
