/* dramsim.h - public API of the lab4 DRAM model ("dramsim").
 *
 * dramsim is a deterministic, seedable model of one DDR4-like memory module behind a simple
 * open-page memory controller. It is the only place in this lab where Rowhammer happens: there
 * is no code in this repository that hammers real DRAM.
 *
 * What the model contains (details in README.md, "The simulated module"):
 *   - a physical address space of 4 or 8 GiB mapped to (bank, row, column) by SECRET XOR
 *     functions chosen per seed from realistic patterns (DRAMA-style);
 *   - one row buffer per bank (open-page policy): row hit / closed / conflict latencies plus
 *     noise, occasional interrupt-like spikes and REF stalls;
 *   - periodic refresh: one REF every tREFI, every row refreshed once per tREFW;
 *   - a disturbance model: per-row thresholds, weak cells with true/anti orientation,
 *     data-pattern dependence, coupling to neighbours at distance 1 and (weaker) 2;
 *   - an optional in-DRAM TRR sampler (opts.trr = 1) with secret size;
 *   - a memory-controller hook for mitigations (Part 6).
 *
 * You own one physically contiguous 1 GiB buffer: [info->buf_base, info->buf_base + info->buf_size).
 * All addresses passed to dsim_* are PHYSICAL addresses inside that buffer (as if you had
 * already translated your virtual addresses with Part 1). Every access models a load or store
 * that misses all caches (think "clflush before every access"), so it always reaches DRAM.
 *
 * Latencies are reported in CPU cycles (info->cpu_ghz). Simulated time advances with every call.
 */
#ifndef DRAMSIM_H
#define DRAMSIM_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsim dsim_t;

typedef struct {
    uint64_t seed;      /* everything secret is derived from this */
    int trr;            /* 1 = enable the in-DRAM TRR sampler */
    double noise;       /* latency noise scale: 1.0 = default, 0 = noiseless (debugging only) */
} dsim_opts_t;

/* Public "datasheet" facts. Nothing here gives away the address mapping. */
typedef struct {
    uint64_t mem_bytes;          /* size of the simulated physical address space */
    uint64_t buf_base, buf_size; /* the physically contiguous buffer you may access */
    unsigned nbanks;             /* number of distinct (channel, rank, bank) combinations */
    unsigned rows_per_bank;
    unsigned row_bytes;          /* bytes per row (per bank), i.e. the row-buffer size */
    double cpu_ghz;              /* latency unit: cycles = ns * cpu_ghz */
    double tREFI_ns, tREFW_ns, tRFC_ns, tRC_ns;
    unsigned refs_per_window;    /* REF commands per tREFW */
    uint64_t max_act_per_window; /* upper bound on ACTs to ONE bank in one tREFW (= tREFW / tRC) */
    uint64_t mac;                /* "maximum activate count": no cell flips before it has received
                                    this much effective disturbance since its last refresh */
    int blast_radius;            /* rows at distance <= blast_radius from an aggressor are disturbed */
    double w2_max;               /* upper bound on the coupling weight at distance 2 (distance 1 = 1.0) */
} dsim_info_t;

typedef struct {
    uint64_t accesses;      /* dsim_access + dsim_read64 + dsim_write64 calls */
    uint64_t row_hits, row_closed, row_conflicts;
    uint64_t acts;          /* row activations caused by accesses */
    uint64_t refs;          /* periodic REF commands executed */
    uint64_t trr_refreshes; /* victim rows refreshed by the in-DRAM TRR */
    uint64_t mc_refreshes;  /* rows refreshed through dsim_mc_refresh (Part 6) */
    uint64_t flips;         /* bit flips that happened anywhere in the module */
    uint64_t violations;    /* accesses outside your buffer (ignored, return 0) */
} dsim_stats_t;

/* A set of candidate bank functions (Part 3, easy mode). mask[i] has bit k set if physical
 * address bit k is XORed into bank-address bit i. */
typedef struct {
    int n;
    uint64_t mask[8];
} dsim_fnset_t;

dsim_t *dsim_create(const dsim_opts_t *opts);
void dsim_destroy(dsim_t *s);
const dsim_info_t *dsim_info(const dsim_t *s);

/* One uncached load from physical address pa. Returns the observed latency in cycles. */
uint32_t dsim_access(dsim_t *s, uint64_t pa);
/* Uncached 64-bit data accesses (pa must be 8-byte aligned). They are timed like dsim_access
 * and activate rows like any other access. */
uint64_t dsim_read64(dsim_t *s, uint64_t pa);
void dsim_write64(dsim_t *s, uint64_t pa, uint64_t value);

/* Let simulated time pass without touching memory (refresh keeps running). */
void dsim_idle(dsim_t *s, double ns);
double dsim_now_ns(const dsim_t *s);
void dsim_get_stats(const dsim_t *s, dsim_stats_t *out);

/* Part 3 easy mode: three candidate function sets, exactly one of which is the real one. */
void dsim_candidates(const dsim_t *s, dsim_fnset_t out[3]);

/* ---------------- Part 6: memory-controller mitigation hook ----------------
 * The controller knows its own mapping, so callbacks receive the internal bank index
 * (0 .. nbanks-1) and row index (0 .. rows_per_bank-1). Rows r-1 and r+1 of the same bank are
 * physically adjacent.
 *   on_activate: called for every ACT caused by an access (not for refreshes).
 *   on_window:   called at the start of every refresh window (every tREFW), e.g. to reset tables.
 * Inside a callback you may call dsim_mc_refresh() to refresh (activate + restore) one row;
 * each such refresh keeps the bank busy for tRC and is counted in stats.mc_refreshes.
 * Out-of-range rows are ignored. Pass NULL to dsim_set_mitigation() to remove a mitigation. */
typedef struct {
    void *state;
    void (*on_activate)(void *state, dsim_t *s, unsigned bank, unsigned row);
    void (*on_window)(void *state, dsim_t *s);
} dsim_mitigation_t;

void dsim_set_mitigation(dsim_t *s, const dsim_mitigation_t *m);
void dsim_mc_refresh(dsim_t *s, unsigned bank, unsigned row);

#ifdef __cplusplus
}
#endif
#endif
