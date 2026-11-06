/* Part 3 - REFERENCE SOLUTION (spoilers). Eviction sets from huge pages; prime and probe. */
#include "lab.h"

int pool_init(pool_t *pool, size_t bytes) {
    pool->base = plat_alloc(bytes, 1);
    pool->bytes = bytes;
    pool->huge = plat_is_huge(pool->base);
    return pool->base ? 0 : -1;
}

/* With 2 MiB pages, PA bits 0..20 are known, so the L2 set index (PA bits 6..6+log2(sets)) is the
 * same as VA bits 6..: lines congruent in L2 are exactly l2_sets*line bytes apart. */
int evset_build(const pool_t *pool, unsigned set, int n, uint8_t **lines) {
    const plat_geom_t *g = plat_geom();
    if (!pool->huge) return 0; /* see README "Going further" for small pages */
    size_t way_stride = (size_t)g->l2_sets * (size_t)g->line;
    int k = 0;
    for (size_t off = (size_t)set * (size_t)g->line; off < pool->bytes && k < n; off += way_stride)
        lines[k++] = pool->base + off;
    return k;
}

void pp_prime(uint8_t *const *lines, int n) {
    /* forward then backward: with tree-PLRU a single sweep may leave stale victim lines behind */
    for (int i = 0; i < n; i++) plat_access(lines[i]);
    for (int i = n - 1; i >= 0; i--) plat_access(lines[i]);
}

int pp_probe(uint8_t *const *lines, int n, const cal_t *cal) {
    int miss = 0;
    for (int i = 0; i < n; i++) miss += plat_probe(lines[i]) > cal->l2_miss;
    return miss;
}
