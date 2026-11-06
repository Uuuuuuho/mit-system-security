/* Part 3 - eviction sets, prime and probe.                       (Exercise 3-1)
 * These helpers are reused by Parts 4 and 5, so keep them general. */
#include "lab.h"

int pool_init(pool_t *pool, size_t bytes) {
    /* Given: ask for huge pages and remember whether we really got them. */
    pool->base = plat_alloc(bytes, 1);
    pool->bytes = bytes;
    pool->huge = plat_is_huge(pool->base);
    return pool->base ? 0 : -1;
}

int evset_build(const pool_t *pool, unsigned set, int n, uint8_t **lines) {
    /* TODO (Exercise 3-1): fill lines[0..n) with pool addresses that map to L2 set `set`.
     * With huge pages this is pure address arithmetic (which VA bits form the L2 set index?).
     * Return the number of lines found. */
    (void)pool;
    (void)set;
    (void)n;
    (void)lines;
    return 0;
}

void pp_prime(uint8_t *const *lines, int n) {
    /* TODO (Exercise 3-1): load every line so the set is full of YOUR lines.
     * Hint: with tree-PLRU replacement one sweep may not be enough. */
    (void)lines;
    (void)n;
}

int pp_probe(uint8_t *const *lines, int n, const cal_t *cal) {
    /* TODO (Exercise 3-1): time every line; return how many are slower than cal->l2_miss. */
    (void)lines;
    (void)n;
    (void)cal;
    return 0;
}
