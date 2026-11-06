/* sim_cache.h - set-associative cache model and a small inclusive hierarchy for the sim backend. */
#ifndef SIM_CACHE_H
#define SIM_CACHE_H
#include <stdint.h>

enum { REPL_LRU = 0, REPL_PLRU = 1, REPL_RANDOM = 2 };
enum { LVL_HIT_L1 = 1, LVL_HIT_L2 = 2, LVL_HIT_LLC = 3, LVL_HIT_DRAM = 4 };

#define SIM_MAX_CORES 2
#define SIM_MAX_HARTS 3 /* hart 0/1 = SMT threads, hart 2 = "OS" noise */

typedef struct {
    int sets, ways, repl;
    int set_bits;
    uint64_t *tag;   /* sets*ways; line address + 1, 0 = invalid                */
    uint64_t *stamp; /* LRU timestamps                                          */
    uint32_t *plru;  /* one tree-PLRU bit vector per set (ways-1 bits, heap)    */
    uint64_t tick;
    uint64_t rng;
} cache_t;

void cache_init(cache_t *c, int sets, int ways, int repl, uint64_t seed);
int cache_lookup(cache_t *c, uint64_t line, int touch); /* way index or -1 */
/* Insert `line` (must be absent). Only ways [lo, hi) may be replaced. Returns evicted line+1 or 0. */
uint64_t cache_fill(cache_t *c, uint64_t line, int lo, int hi);
void cache_invalidate(cache_t *c, uint64_t line);

typedef struct {
    int stride_pf;   /* L2 stride prefetcher (trains on L1 misses, stays inside a 4 KiB page) */
    int adjacent_pf; /* on a DRAM fill also bring the 128-byte buddy line into the LLC          */
    int partition;   /* way-partition L1/L2 between hart 0 and hart 1 (defense experiment)      */
} hier_opts_t;

typedef struct {
    uint64_t page, last_off;
    int64_t stride;
    int conf;
    uint64_t used;
} pf_entry_t;

typedef struct {
    int ncores;
    cache_t l1[SIM_MAX_CORES], l2[SIM_MAX_CORES], llc;
    hier_opts_t opt;
    pf_entry_t pf[SIM_MAX_CORES][16];
    uint64_t pf_tick;
    uint64_t stat_access[5];
} hier_t;

void hier_init(hier_t *h, int ncores, int l1s, int l1w, int l1r, int l2s, int l2w, int l2r, int llcs,
               int llcw, int llcr, hier_opts_t opt, uint64_t seed);
/* Demand load of physical address pa by (core, hart). Returns the level that served it. */
int hier_access(hier_t *h, int core, int hart, uint64_t pa);
/* Remove the line from every level of every core. */
void hier_flush(hier_t *h, uint64_t pa);
/* Where is the line right now for this core? (oracle used only by graders) */
int hier_where(hier_t *h, int core, uint64_t pa);

#endif
