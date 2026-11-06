/* part3_xor.c - Part 3: recovering the bank address functions.        (YOUR CODE GOES HERE)
 *
 * Exercise 3-1  pick_candidate      (easy: one of three candidate sets is right)
 * Exercise 3-2  recover_bank_fns    (challenge: no candidates)
 * Exercise 3-3  find_row_lo
 *
 * Build and test:  make && ./build/part3 --seed 1
 * Your timing primitive is your own Part 2 code: same_bank_diff_row(sim, a, b, threshold).
 * Notation: a function "a13^a18" means bank bit = pa[13] XOR pa[18], i.e. mask (1<<13)|(1<<18).
 */
#include <stdlib.h>
#include <string.h>

#include "../include/lab4.h"

/* ---- helpers you may use ---- */
static uint64_t next_rand(uint64_t *st) { /* splitmix64 */
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

/* Gaussian elimination over GF(2), pivoting on the LOWEST bit: afterwards m[0..rank-1] is a basis
 * of the same span in which mask i contains bit piv[i] and no other mask contains it.
 * (Flipping address bit piv[i] therefore toggles bank bit i and nothing else.) Returns the rank. */
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

/* ---------------------------------------------------------------- Exercise 3-1 */

int pick_candidate(dsim_t *sim, const dsim_fnset_t cands[3], double threshold, uint64_t seed) {
    /* TODO: for each candidate, construct address pairs that the candidate predicts to be in the
     * same bank (but in different rows) and test them with same_bank_diff_row(). The right
     * candidate's predictions come true. Bits of a mask above your buffer are constant inside it. */
    uint64_t st = seed;
    (void)sim; (void)cands; (void)threshold; (void)st; (void)rand_line; (void)parity; (void)reduce_low;
    return -1;
}

/* ---------------------------------------------------------------- Exercise 3-2 */

int recover_bank_fns(dsim_t *sim, double threshold, uint64_t seed, uint64_t *masks, int max_masks) {
    /* TODO: (1) fix a base address a0 and collect addresses x that conflict with it (same bank,
     * different row). Be paranoid: one false positive ruins step 2.
     * (2) every bank function f satisfies parity(f & (a0 ^ x)) == 0 for all of them. The set of
     * such f is the null space (over GF(2)) of the difference vectors a0 ^ x, restricted to the
     * bits that vary inside your buffer (6 .. log2(buf_size)-1). How many functions do you expect?
     * (dsim_info(sim)->nbanks). Write them to masks[] and return the count. */
    uint64_t st = seed;
    (void)sim; (void)threshold; (void)st; (void)masks; (void)max_masks; (void)log2u;
    return 0;
}

/* ---------------------------------------------------------------- Exercise 3-3 */

int find_row_lo(dsim_t *sim, double threshold, const uint64_t *masks, int nmasks, uint64_t seed) {
    /* TODO: flip one address bit b, then flip pivot bits (see reduce_low) so the bank stays the
     * same. If that pair conflicts, b is a row bit. The row index is made of the high bits;
     * return the lowest one. */
    uint64_t st = seed;
    (void)sim; (void)threshold; (void)masks; (void)nmasks; (void)st;
    return -1;
}
