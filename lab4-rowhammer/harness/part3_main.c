/* part3_main.c - harness for Part 3 (bank function recovery). Provided; do not edit.
 *
 *   ./build/part3 [--seed N] [-v] [--reveal]
 * Uses your Part 2 code (calibrate_threshold, same_bank_diff_row) as the timing primitive.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcommon.h"

static int rank_of(const uint64_t *v, int n) {
    uint64_t m[32];
    int r = 0;
    for (int i = 0; i < n && i < 32; i++) m[i] = v[i];
    for (int bit = 63; bit >= 0; bit--) {
        int p = -1;
        for (int i = r; i < n; i++) if ((m[i] >> bit) & 1) { p = i; break; }
        if (p < 0) continue;
        uint64_t t = m[p]; m[p] = m[r]; m[r] = t;
        for (int i = 0; i < n; i++) if (i != r && ((m[i] >> bit) & 1)) m[i] ^= m[r];
        r++;
    }
    return r;
}

static void print_set(const char *label, const uint64_t *m, int n) {
    printf("  %s", label);
    for (int i = 0; i < n; i++) { printf(i ? ", " : " "); hprint_mask(m[i]); }
    printf("\n");
}

int main(int argc, char **argv) {
    hargs_t a;
    hargs_parse(argc, argv, &a, NULL);
    hres_t r = {3, 0, 0};
    dsim_t *s = hsim(a.seed, 0);
    const dsim_info_t *in = dsim_info(s);
    dsim_truth_t t;
    dsim_oracle_truth(s, &t);
    printf("== Part 3: bank address functions  (%u banks -> %d functions; buffer bits 6..%d vary)\n", in->nbanks,
           __builtin_ctz(in->nbanks), __builtin_ctzll(in->buf_size) - 1);

    double thr = calibrate_threshold(s, a.seed);
    printf("  threshold from your Part 2 code: %.1f cycles\n", thr);
    dsim_stats_t s0, s1;

    /* ---- Exercise 3-1 (easy): pick one of three candidates ---- */
    dsim_fnset_t c[3];
    dsim_candidates(s, c);
    for (int k = 0; k < 3; k++) {
        char lab[32];
        snprintf(lab, sizeof lab, "candidate %d:", k);
        print_set(lab, c[k].mask, c[k].n);
    }
    dsim_get_stats(s, &s0);
    int idx = pick_candidate(s, c, thr, a.seed);
    dsim_get_stats(s, &s1);
    hcheck(&r, idx == t.cand_true, "pick_candidate (easy)", "you picked %d using %llu accesses", idx,
           (unsigned long long)(s1.accesses - s0.accesses));

    /* ---- Exercise 3-2 (challenge): recover the functions ---- */
    uint64_t m[8] = {0}, tr[8], both[16];
    dsim_get_stats(s, &s0);
    int n = recover_bank_fns(s, thr, a.seed, m, 8);
    dsim_get_stats(s, &s1);
    if (n < 0) n = 0;
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) m[i] = dsim_oracle_restrict(s, m[i]);
    print_set("recovered:", m, n);
    for (int i = 0; i < t.nfns; i++) tr[i] = dsim_oracle_restrict(s, t.fn[i]);
    memcpy(both, tr, sizeof(uint64_t) * (size_t)t.nfns);
    memcpy(both + t.nfns, m, sizeof(uint64_t) * (size_t)n);
    int rt = rank_of(tr, t.nfns), rm = rank_of(m, n), rb = rank_of(both, t.nfns + n);
    int span_ok = (n == t.nfns) && rm == rt && rb == rt;
    hcheck(&r, span_ok, "recover_bank_fns (challenge)", "%d function(s), span %s, %llu accesses", n,
           span_ok ? "matches" : "differs", (unsigned long long)(s1.accesses - s0.accesses));

    /* ---- Exercise 3-3: lowest row bit ---- */
    const uint64_t *use = span_ok ? m : tr;
    if (!span_ok) printf("  (find_row_lo gets the true functions because 3-2 did not pass)\n");
    dsim_get_stats(s, &s0);
    int rl = find_row_lo(s, thr, use, t.nfns, a.seed);
    dsim_get_stats(s, &s1);
    hcheck(&r, rl == t.row_lo, "find_row_lo", "%d, %llu accesses", rl, (unsigned long long)(s1.accesses - s0.accesses));

    if (a.reveal) {
        printf("  [reveal] template %s: ", t.template_name);
        for (int i = 0; i < t.nfns; i++) { printf(" "); hprint_mask(t.fn[i]); }
        printf("   row bits a%d..a%d, candidate %d\n", t.row_lo, t.addr_bits - 1, t.cand_true);
    }
    dsim_destroy(s);
    return hfinish(&r);
}
