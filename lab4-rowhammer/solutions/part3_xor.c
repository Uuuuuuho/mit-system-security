/* part3_xor.c - REFERENCE SOLUTION for Part 3. SPOILER: try the lab yourself first.
 *
 * Exercise 3-1  pick_candidate     build pairs that each candidate predicts to conflict; count hits
 * Exercise 3-2  recover_bank_fns   collect same-bank addresses, the functions span the GF(2)
 *                                  null space of their pairwise differences
 * Exercise 3-3  find_row_lo        flip one bit, compensate the bank with pivot bits, look for
 *                                  a conflict
 */
#include <stdlib.h>
#include <string.h>

#include "../include/lab4.h"

#define LINE_BITS 6

static uint64_t next_rand(uint64_t *st) {
    uint64_t z = (*st += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint64_t rand_line(const dsim_info_t *in, uint64_t *st) {
    return in->buf_base + (next_rand(st) % in->buf_size & ~63ull);
}
static int parity(uint64_t x) { return __builtin_parityll(x); }
static int log2u(uint64_t x) { return 63 - __builtin_clzll(x); }

/* Majority vote over three timing decisions: makes a false positive very unlikely. */
static int conflict3(dsim_t *sim, uint64_t a, uint64_t b, double thr) {
    int v = same_bank_diff_row(sim, a, b, thr);
    v += same_bank_diff_row(sim, a, b, thr);
    if (v == 1) v += same_bank_diff_row(sim, a, b, thr);
    return v >= 2;
}

/* Reduce a set of masks to a basis in which every mask owns a private "pivot" bit (its lowest
 * bit, cleared in all other masks). Returns the rank; pivots in piv[]. */
static int reduce_low(uint64_t *m, int n, int *piv) {
    int r = 0;
    for (int bit = 0; bit < 64 && r < n; bit++) {
        int p = -1;
        for (int i = r; i < n; i++) if ((m[i] >> bit) & 1) { p = i; break; }
        if (p < 0) continue;
        uint64_t t = m[p]; m[p] = m[r]; m[r] = t;
        for (int i = 0; i < n; i++) if (i != r && ((m[i] >> bit) & 1)) m[i] ^= m[r];
        piv[r++] = bit;
    }
    return r;
}

/* Make b agree with a on every function by flipping pivot bits (masks must be reduced). */
static uint64_t same_bank_as(uint64_t a, uint64_t b, const uint64_t *m, const int *piv, int n) {
    for (int i = 0; i < n; i++)
        if (parity(a & m[i]) != parity(b & m[i])) b ^= 1ull << piv[i];
    return b;
}

int pick_candidate(dsim_t *sim, const dsim_fnset_t cands[3], double threshold, uint64_t seed) {
    const dsim_info_t *in = dsim_info(sim);
    uint64_t st = seed ^ 0xEA5E;
    int best = 0, best_score = -1;
    uint64_t keep = (in->buf_size - 1) & ~63ull; /* bits we may change inside the buffer */
    for (int k = 0; k < 3; k++) {
        uint64_t m[8];
        int piv[8], n = cands[k].n;
        for (int i = 0; i < n; i++) m[i] = cands[k].mask[i] & keep; /* constant bits drop out */
        n = reduce_low(m, n, piv);
        int score = 0;
        for (int t = 0; t < 32; t++) {
            uint64_t a = rand_line(in, &st);
            uint64_t b = same_bank_as(a, rand_line(in, &st), m, piv, n);
            score += same_bank_diff_row(sim, a, b, threshold);
        }
        if (score > best_score) { best_score = score; best = k; }
    }
    return best;
}

int recover_bank_fns(dsim_t *sim, double threshold, uint64_t seed, uint64_t *masks, int max_masks) {
    const dsim_info_t *in = dsim_info(sim);
    const int nfns = log2u(in->nbanks);
    const int hi = log2u(in->buf_size);          /* bits LINE_BITS .. hi-1 vary */
    const int W = hi - LINE_BITS;
    uint64_t st = seed ^ 0x5EC2E7;
    if (max_masks < nfns) return -1;

    for (int attempt = 0; attempt < 8; attempt++) {
        uint64_t a0 = rand_line(in, &st);
        uint64_t rows[64]; /* RREF of the difference vectors, kept incrementally */
        int rank = 0, found = 0;
        for (int tries = 0; tries < 20000 && W - rank > nfns; tries++) {
            uint64_t x = rand_line(in, &st);
            if (!conflict3(sim, a0, x, threshold)) continue;
            found++;
            uint64_t d = ((a0 ^ x) >> LINE_BITS) & ((1ull << W) - 1);
            /* insert into the row-echelon basis */
            for (int i = 0; i < rank; i++) {
                int lead = 63 - __builtin_clzll(rows[i]);
                if ((d >> lead) & 1) d ^= rows[i];
            }
            if (!d) continue;
            int lead = 63 - __builtin_clzll(d);
            for (int i = 0; i < rank; i++) if ((rows[i] >> lead) & 1) rows[i] ^= d;
            rows[rank++] = d;
        }
        if (W - rank != nfns) continue; /* not enough samples, or a false positive slipped in */

        /* null space: one vector per free column f; pivot columns copy the bit of column f */
        int is_lead[64] = {0}, lead_row[64];
        for (int i = 0; i < rank; i++) {
            int lead = 63 - __builtin_clzll(rows[i]);
            is_lead[lead] = 1;
            lead_row[lead] = i;
        }
        int n = 0;
        uint64_t ns[64];
        for (int f = 0; f < W; f++) {
            if (is_lead[f]) continue;
            uint64_t v = 1ull << f;
            for (int c = 0; c < W; c++)
                if (is_lead[c] && ((rows[lead_row[c]] >> f) & 1)) v |= 1ull << c;
            ns[n++] = v;
        }
        /* sanity: every difference must be orthogonal to every null-space vector */
        int ok = 1;
        for (int i = 0; i < rank && ok; i++)
            for (int j = 0; j < n; j++) if (parity(rows[i] & ns[j])) ok = 0;
        if (!ok) continue;

        /* prettier basis: greedily take the lowest-weight combinations (DRAMA-style output) */
        int out = 0;
        uint64_t chosen[8];
        for (int w = 1; w <= W && out < nfns; w++)
            for (uint32_t c = 1; c < (1u << n) && out < nfns; c++) {
                uint64_t v = 0;
                for (int j = 0; j < n; j++) if ((c >> j) & 1) v ^= ns[j];
                if (__builtin_popcountll(v) != w) continue;
                /* independent of what we already have? */
                uint64_t tmp[9];
                memcpy(tmp, chosen, sizeof(uint64_t) * (size_t)out);
                tmp[out] = v;
                int piv[9];
                if (reduce_low(tmp, out + 1, piv) == out + 1) chosen[out++] = v;
            }
        for (int i = 0; i < out; i++) masks[i] = chosen[i] << LINE_BITS;
        (void)found;
        return out;
    }
    return 0;
}

int find_row_lo(dsim_t *sim, double threshold, const uint64_t *masks, int nmasks, uint64_t seed) {
    const dsim_info_t *in = dsim_info(sim);
    const int hi = log2u(in->buf_size);
    uint64_t m[8], st = seed ^ 0x20311;
    int piv[8];
    if (nmasks > 8) nmasks = 8;
    for (int i = 0; i < nmasks; i++) m[i] = masks[i] & (in->buf_size - 1);
    int n = reduce_low(m, nmasks, piv);
    for (int b = LINE_BITS; b < hi; b++) {
        int is_piv = 0;
        for (int i = 0; i < n; i++) if (piv[i] == b) is_piv = 1;
        if (is_piv) continue; /* pivots are bank/column bits by construction */
        int votes = 0;
        for (int t = 0; t < 5; t++) {
            uint64_t a = rand_line(in, &st);
            uint64_t x = a ^ (1ull << b);
            for (int i = 0; i < n; i++) if ((m[i] >> b) & 1) x ^= 1ull << piv[i]; /* keep the bank */
            votes += same_bank_diff_row(sim, a, x, threshold);
        }
        if (votes >= 3) return b; /* flipping b changed the row: lowest row bit found */
    }
    return -1;
}
