/* hcommon.c - shared helpers for the lab4 harness programs (provided). */
#include "hcommon.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void hargs_parse(int argc, char **argv, hargs_t *a, const char *usage) {
    memset(a, 0, sizeof *a);
    a->mode = "auto";
    const char *env = getenv("SEED");
    a->seed = env ? strtoull(env, NULL, 0) : (uint64_t)time(NULL);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) a->seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) a->mode = argv[++i];
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) a->verbose = 1;
        else if (!strcmp(argv[i], "--reveal")) a->reveal = 1;
        else {
            fprintf(stderr, "usage: %s [--seed N] [-v] [--reveal]%s\n", argv[0], usage ? usage : "");
            exit(2);
        }
    }
    printf("seed=%llu\n", (unsigned long long)a->seed);
}

void hcheck(hres_t *r, int ok, const char *name, const char *fmt, ...) {
    r->total++;
    if (ok) r->passed++;
    printf("  [%s] %s", ok ? "PASS" : "FAIL", name);
    if (fmt && *fmt) {
        va_list ap;
        va_start(ap, fmt);
        printf(": ");
        vprintf(fmt, ap);
        va_end(ap);
    }
    printf("\n");
    fflush(stdout);
}

int hfinish(hres_t *r) {
    int ok = r->total > 0 && r->passed == r->total;
    printf("RESULT part=%d status=%s passed=%d total=%d\n", r->part, ok ? "PASS" : "FAIL", r->passed, r->total);
    fflush(stdout);
    return ok ? 0 : 1;
}

uint64_t hrand(uint64_t *st) {
    uint64_t z = (*st += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

dsim_t *hsim(uint64_t seed, int trr) {
    dsim_opts_t o = {seed, trr, 1.0};
    dsim_t *s = dsim_create(&o);
    if (!s) { fprintf(stderr, "dsim_create failed\n"); exit(1); }
    return s;
}

void hbuf_rows(const dsim_t *s, unsigned *first, unsigned *count) {
    dsim_truth_t t;
    dsim_oracle_truth(s, &t);
    const dsim_info_t *in = dsim_info(s);
    *first = (unsigned)((in->buf_base >> t.row_lo) & (in->rows_per_bank - 1));
    *count = (unsigned)(in->buf_size >> t.row_lo);
}

void hmap_from_oracle(const dsim_t *s, dram_map_t *m) {
    dsim_truth_t t;
    dsim_oracle_truth(s, &t);
    const dsim_info_t *in = dsim_info(s);
    memset(m, 0, sizeof *m);
    m->nfns = t.nfns;
    for (int i = 0; i < t.nfns; i++) { m->fn[i] = t.fn[i]; m->pivot[i] = t.pivot[i]; }
    m->row_lo = t.row_lo;
    m->rows_per_bank = in->rows_per_bank;
    m->col_mask = t.col_mask;
    m->buf_base = in->buf_base;
    m->buf_size = in->buf_size;
    hbuf_rows(s, &m->row_first, &m->row_count);
}

void hprint_mask(uint64_t mask) {
    int first = 1;
    for (int b = 0; b < 64; b++)
        if ((mask >> b) & 1) { printf("%sa%d", first ? "" : "^", b); first = 0; }
    if (first) printf("0");
}

void href_fill(dsim_t *s, unsigned bank, unsigned lo, unsigned hi, const unsigned *aggr, int n, uint64_t pattern) {
    unsigned rb = dsim_info(s)->row_bytes;
    for (unsigned r = lo; r <= hi; r++) {
        int is_aggr = 0;
        for (int i = 0; i < n; i++) if (aggr[i] == r) is_aggr = 1;
        uint64_t v = is_aggr ? ~pattern : pattern;
        for (unsigned c = 0; c < rb; c += 8) dsim_write64(s, dsim_oracle_compose(s, bank, r, c), v);
    }
}

void href_hammer(dsim_t *s, unsigned bank, const unsigned *aggr, int n, double ns) {
    uint64_t pa[64];
    for (int i = 0; i < n && i < 64; i++) pa[i] = dsim_oracle_compose(s, bank, aggr[i], 0);
    double end = dsim_now_ns(s) + ns;
    while (dsim_now_ns(s) < end)
        for (int i = 0; i < n && i < 64; i++) dsim_access(s, pa[i]);
}
