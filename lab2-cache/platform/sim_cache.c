/* sim_cache.c - set-associative caches (LRU / tree-PLRU / random) and an inclusive 3-level hierarchy.
 *
 * Hierarchy rules (simple and documented in the README):
 *   - L1 and L2 are private per core and shared by the two SMT harts of a core.
 *   - The LLC is shared by all cores. Every level is INCLUSIVE of the one above it:
 *       an L2 eviction removes the line from that core's L1,
 *       an LLC eviction removes the line from every core's L1 and L2 ("back-invalidation").
 *   - Set index = line address mod sets (line address = PA >> 6). No slice hashing.
 *   - Optional prefetchers: an L2 stride prefetcher per core (trains on L1 misses, never crosses a
 *     4 KiB page) and an adjacent-line prefetcher that pulls the 128-byte buddy into the LLC.
 */
#include "sim_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ilog2(int x) {
    int b = 0;
    while ((1 << b) < x) b++;
    return b;
}

void cache_init(cache_t *c, int sets, int ways, int repl, uint64_t seed) {
    memset(c, 0, sizeof *c);
    c->sets = sets;
    c->ways = ways;
    c->repl = repl;
    c->set_bits = ilog2(sets);
    if ((1 << c->set_bits) != sets || ways > 32 || (repl == REPL_PLRU && (ways & (ways - 1)))) {
        fprintf(stderr, "sim: bad cache geometry sets=%d ways=%d\n", sets, ways);
        exit(2);
    }
    c->tag = calloc((size_t)sets * ways, sizeof *c->tag);
    c->stamp = calloc((size_t)sets * ways, sizeof *c->stamp);
    c->plru = calloc((size_t)sets, sizeof *c->plru);
    if (!c->tag || !c->stamp || !c->plru) { perror("calloc"); exit(2); }
    c->rng = seed | 1;
}

/* ---- tree-PLRU: ways-1 internal nodes in heap order; bit=1 means "next victim is on the right" */
static void plru_touch(uint32_t *bits, int ways, int w) {
    int node = 0, lo = 0, hi = ways;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (w < mid) { *bits |= 1u << node; node = 2 * node + 1; hi = mid; }
        else { *bits &= ~(1u << node); node = 2 * node + 2; lo = mid; }
    }
}
static int plru_victim(uint32_t bits, int ways, int alo, int ahi) {
    int node = 0, lo = 0, hi = ways;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2, right;
        if (ahi <= mid) right = 0;       /* allowed ways all on the left  */
        else if (alo >= mid) right = 1;  /* allowed ways all on the right */
        else right = (bits >> node) & 1;
        if (right) { node = 2 * node + 2; lo = mid; }
        else { node = 2 * node + 1; hi = mid; }
    }
    return lo;
}

static inline void touch(cache_t *c, int set, int w) {
    c->stamp[(size_t)set * c->ways + w] = ++c->tick;
    if (c->repl == REPL_PLRU) plru_touch(&c->plru[set], c->ways, w);
}

int cache_lookup(cache_t *c, uint64_t line, int do_touch) {
    int set = (int)(line & (uint64_t)(c->sets - 1));
    uint64_t *t = &c->tag[(size_t)set * c->ways], key = line + 1;
    for (int w = 0; w < c->ways; w++)
        if (t[w] == key) {
            if (do_touch) touch(c, set, w);
            return w;
        }
    return -1;
}

uint64_t cache_fill(cache_t *c, uint64_t line, int lo, int hi) {
    int set = (int)(line & (uint64_t)(c->sets - 1));
    size_t base = (size_t)set * c->ways;
    uint64_t *t = &c->tag[base];
    int victim = -1;
    for (int w = lo; w < hi; w++)
        if (t[w] == 0) { victim = w; break; }
    if (victim < 0) {
        if (c->repl == REPL_PLRU) {
            victim = plru_victim(c->plru[set], c->ways, lo, hi);
        } else if (c->repl == REPL_RANDOM) {
            c->rng ^= c->rng << 13; c->rng ^= c->rng >> 7; c->rng ^= c->rng << 17;
            victim = lo + (int)(c->rng % (uint64_t)(hi - lo));
        } else {
            uint64_t best = UINT64_MAX;
            for (int w = lo; w < hi; w++)
                if (c->stamp[base + w] < best) { best = c->stamp[base + w]; victim = w; }
        }
    }
    uint64_t old = t[victim];
    t[victim] = line + 1;
    touch(c, set, victim);
    return old; /* line+1 of the evicted line, or 0 */
}

void cache_invalidate(cache_t *c, uint64_t line) {
    int w = cache_lookup(c, line, 0);
    if (w >= 0) {
        int set = (int)(line & (uint64_t)(c->sets - 1));
        c->tag[(size_t)set * c->ways + w] = 0;
        c->stamp[(size_t)set * c->ways + w] = 0;
    }
}

/* ------------------------------------------------------------------ hierarchy */
void hier_init(hier_t *h, int ncores, int l1s, int l1w, int l1r, int l2s, int l2w, int l2r, int llcs,
               int llcw, int llcr, hier_opts_t opt, uint64_t seed) {
    memset(h, 0, sizeof *h);
    h->ncores = ncores;
    h->opt = opt;
    for (int c = 0; c < ncores; c++) {
        cache_init(&h->l1[c], l1s, l1w, l1r, seed + 11 + c);
        cache_init(&h->l2[c], l2s, l2w, l2r, seed + 23 + c);
    }
    cache_init(&h->llc, llcs, llcw, llcr, seed + 37);
}

static void range_for(const hier_t *h, const cache_t *c, int hart, int *lo, int *hi) {
    *lo = 0;
    *hi = c->ways;
    if (h->opt.partition && hart < 2) {
        *lo = hart == 0 ? 0 : c->ways / 2;
        *hi = hart == 0 ? c->ways / 2 : c->ways;
    }
}

static void fill_llc(hier_t *h, uint64_t line) {
    uint64_t ev = cache_fill(&h->llc, line, 0, h->llc.ways);
    if (ev) /* inclusive LLC: back-invalidate every private copy */
        for (int c = 0; c < h->ncores; c++) {
            cache_invalidate(&h->l2[c], ev - 1);
            cache_invalidate(&h->l1[c], ev - 1);
        }
}
static void fill_l2(hier_t *h, int core, int hart, uint64_t line) {
    int lo, hi;
    range_for(h, &h->l2[core], hart, &lo, &hi);
    uint64_t ev = cache_fill(&h->l2[core], line, lo, hi);
    if (ev) cache_invalidate(&h->l1[core], ev - 1); /* L2 is inclusive of L1 */
}
static void fill_l1(hier_t *h, int core, int hart, uint64_t line) {
    int lo, hi;
    range_for(h, &h->l1[core], hart, &lo, &hi);
    cache_fill(&h->l1[core], line, lo, hi);
}

static void prefetch_l2(hier_t *h, int core, int hart, uint64_t line) {
    if (cache_lookup(&h->l2[core], line, 0) >= 0) return;
    if (cache_lookup(&h->llc, line, 1) < 0) fill_llc(h, line);
    fill_l2(h, core, hart, line);
}

/* Stride prefetcher: 16 entries per core, keyed by 4 KiB page. After two equal strides inside a page
 * it prefetches the next two lines along the stride into L2. */
static void pf_train(hier_t *h, int core, int hart, uint64_t line) {
    if (!h->opt.stride_pf) return;
    uint64_t page = line >> 6, off = line & 63;
    pf_entry_t *tab = h->pf[core], *e = NULL, *lru = &tab[0];
    for (int i = 0; i < 16; i++) {
        if (tab[i].used && tab[i].page == page) { e = &tab[i]; break; }
        if (tab[i].used < lru->used) lru = &tab[i];
    }
    if (!e) {
        e = lru;
        e->page = page;
        e->last_off = off;
        e->stride = 0;
        e->conf = 0;
        e->used = ++h->pf_tick;
        return;
    }
    e->used = ++h->pf_tick;
    int64_t s = (int64_t)off - (int64_t)e->last_off;
    if (s == 0) return;
    if (s == e->stride) e->conf++;
    else { e->stride = s; e->conf = 0; }
    e->last_off = off;
    if (e->conf >= 1)
        for (int k = 1; k <= 2; k++) {
            int64_t no = (int64_t)off + k * s;
            if (no >= 0 && no < 64) prefetch_l2(h, core, hart, page * 64 + (uint64_t)no);
        }
}

int hier_access(hier_t *h, int core, int hart, uint64_t pa) {
    uint64_t line = pa >> 6;
    int lvl;
    if (cache_lookup(&h->l1[core], line, 1) >= 0) {
        lvl = LVL_HIT_L1;
    } else if (cache_lookup(&h->l2[core], line, 1) >= 0) {
        fill_l1(h, core, hart, line);
        pf_train(h, core, hart, line);
        lvl = LVL_HIT_L2;
    } else if (cache_lookup(&h->llc, line, 1) >= 0) {
        fill_l2(h, core, hart, line);
        fill_l1(h, core, hart, line);
        pf_train(h, core, hart, line);
        lvl = LVL_HIT_LLC;
    } else {
        fill_llc(h, line);
        fill_l2(h, core, hart, line);
        fill_l1(h, core, hart, line);
        if (h->opt.adjacent_pf && cache_lookup(&h->llc, line ^ 1, 0) < 0) fill_llc(h, line ^ 1);
        pf_train(h, core, hart, line);
        lvl = LVL_HIT_DRAM;
    }
    h->stat_access[lvl]++;
    return lvl;
}

void hier_flush(hier_t *h, uint64_t pa) {
    uint64_t line = pa >> 6;
    for (int c = 0; c < h->ncores; c++) {
        cache_invalidate(&h->l1[c], line);
        cache_invalidate(&h->l2[c], line);
    }
    cache_invalidate(&h->llc, line);
}

int hier_where(hier_t *h, int core, uint64_t pa) {
    uint64_t line = pa >> 6;
    if (cache_lookup(&h->l1[core], line, 0) >= 0) return LVL_HIT_L1;
    if (cache_lookup(&h->l2[core], line, 0) >= 0) return LVL_HIT_L2;
    if (cache_lookup(&h->llc, line, 0) >= 0) return LVL_HIT_LLC;
    return LVL_HIT_DRAM;
}
