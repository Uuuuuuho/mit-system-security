/* part2_main.c - harness for Part 2 (bank conflicts by timing). Provided; do not edit.
 *
 *   ./build/part2 [--seed N] [-v] [--reveal]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcommon.h"

static uint64_t rand_line(const dsim_info_t *in, uint64_t *rs) {
    return in->buf_base + (hrand(rs) % in->buf_size & ~63ull);
}

static void histogram(const double *v, int n, double thr) {
    double lo = 1e9, hi = 0;
    for (int i = 0; i < n; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
    if (!(hi > lo)) return;
    int nb = 24, cnt[24] = {0};
    double w = (hi - lo) / nb + 1e-9;
    for (int i = 0; i < n; i++) cnt[(int)((v[i] - lo) / w) < nb ? (int)((v[i] - lo) / w) : nb - 1]++;
    int mx = 1;
    for (int b = 0; b < nb; b++) if (cnt[b] > mx) mx = cnt[b];
    printf("  latency histogram of measure_pair() over %d random pairs (log-scaled bars)\n", n);
    for (int b = 0; b < nb; b++) {
        double from = lo + b * w;
        int len = cnt[b] ? 1 + (int)(40.0 * log(1.0 + cnt[b]) / log(1.0 + mx)) : 0;
        printf("  %7.1f %c %5d |", from, (thr >= from && thr < from + w) ? '<' : ' ', cnt[b]);
        for (int k = 0; k < len; k++) putchar('#');
        putchar('\n');
    }
    printf("  ('<' marks the bucket that contains your threshold %.1f)\n", thr);
}

int main(int argc, char **argv) {
    hargs_t a;
    hargs_parse(argc, argv, &a, NULL);
    hres_t r = {2, 0, 0};
    uint64_t rs = a.seed ^ 0x2222;
    dsim_t *s = hsim(a.seed, 0);
    const dsim_info_t *in = dsim_info(s);
    printf("== Part 2: bank conflicts  (%u banks, %u-byte rows, %.1f GHz cycles)\n", in->nbanks, in->row_bytes, in->cpu_ghz);

    /* ---- Exercise 2-1: the three kinds of pairs ---- */
    uint64_t base = rand_line(in, &rs);
    unsigned bk, rw, cl;
    dsim_oracle_decode(s, base, &bk, &rw, &cl);
    unsigned first, count;
    hbuf_rows(s, &first, &count);
    unsigned rw2 = (rw + 1 < first + count) ? rw + 1 : rw - 1;
    uint64_t same_row = base + 8;
    uint64_t conflict = dsim_oracle_compose(s, bk, rw2, (unsigned)(hrand(&rs) % in->row_bytes) & ~63u);
    uint64_t other_bank = dsim_oracle_compose(s, bk ^ 1u, rw, cl);
    double m_same = measure_pair(s, base, same_row, 20);
    double m_conf = measure_pair(s, base, conflict, 20);
    double m_diff = measure_pair(s, base, other_bank, 20);
    printf("  measure_pair: same row %.1f | other bank %.1f | same bank, other row %.1f cycles\n", m_same, m_diff, m_conf);
    hcheck(&r, m_conf > m_same + 30 && m_conf > m_diff + 30 && fabs(m_same - m_diff) < 30,
           "measure_pair separates conflicts", "gap %.1f cycles", m_conf - (m_same > m_diff ? m_same : m_diff));

    /* ---- Exercise 2-2: threshold ---- */
    double thr = calibrate_threshold(s, a.seed);
    hcheck(&r, thr > (m_same > m_diff ? m_same : m_diff) + 10 && thr < m_conf - 10, "calibrate_threshold",
           "%.1f cycles", thr);

    double hv[600];
    uint64_t hb = rand_line(in, &rs);
    for (int i = 0; i < 600; i++) hv[i] = measure_pair(s, hb, rand_line(in, &rs), 10);
    histogram(hv, 600, thr);

    /* ---- Exercise 2-3: classification ---- */
    dsim_stats_t s0, s1;
    dsim_get_stats(s, &s0);
    int tp = 0, fp = 0, fn = 0, tn = 0;
    for (int k = 0; k < 3; k++) {
        uint64_t b0 = rand_line(in, &rs);
        unsigned b0b, b0r, b0c;
        dsim_oracle_decode(s, b0, &b0b, &b0r, &b0c);
        for (int i = 0; i < 600; i++) {
            uint64_t x;
            if (i % 5 == 0) /* enrich with true positives so recall is measured well */
                x = dsim_oracle_compose(s, b0b, first + (unsigned)(hrand(&rs) % count),
                                        (unsigned)(hrand(&rs) % in->row_bytes) & ~63u);
            else x = rand_line(in, &rs);
            unsigned xb, xr, xc;
            dsim_oracle_decode(s, x, &xb, &xr, &xc);
            int truth = (xb == b0b) && (xr != b0r);
            int pred = same_bank_diff_row(s, b0, x, thr);
            if (pred && truth) tp++;
            else if (pred) fp++;
            else if (truth) fn++;
            else tn++;
        }
    }
    dsim_get_stats(s, &s1);
    int n = tp + fp + fn + tn;
    double acc = (double)(tp + tn) / n, rec = tp + fn ? (double)tp / (tp + fn) : 0, prec = tp + fp ? (double)tp / (tp + fp) : 0;
    printf("  classification: TP=%d FP=%d FN=%d TN=%d  (%.0f accesses per decision)\n", tp, fp, fn, tn,
           (double)(s1.accesses - s0.accesses) / n);
    hcheck(&r, acc >= 0.99, "same_bank_diff_row accuracy", "%.3f (need >= 0.99)", acc);
    hcheck(&r, rec >= 0.95 && prec >= 0.95, "same_bank_diff_row recall/precision", "%.3f / %.3f (need >= 0.95)", rec, prec);
    if (a.reveal) {
        dsim_truth_t t;
        dsim_oracle_truth(s, &t);
        printf("  [reveal] template %s, functions:", t.template_name);
        for (int i = 0; i < t.nfns; i++) { printf(" "); hprint_mask(t.fn[i]); }
        printf(", row_lo %d\n", t.row_lo);
    }
    dsim_destroy(s);
    return hfinish(&r);
}
