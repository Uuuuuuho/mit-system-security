/* lab4.h - the functions YOU implement in src/partN_*.c, and the types they use.
 *
 * The harness programs (harness/partN_main.c) call these functions; `make check` grades them.
 * Do not change the prototypes. Feel free to add static helpers in your own files.
 */
#ifndef LAB4_H
#define LAB4_H
#include <stddef.h>
#include <stdint.h>

#include "../dramsim/dramsim.h"
#include "../support/pagemap.h"

/* =========================================================================================
 * Part 1 - virtual -> physical -> virtual  (native when permitted, simulated pagemap otherwise)
 * ========================================================================================= */

/* Exercise 1-1. Decode one raw pagemap entry for virtual address `vaddr`.
 * On success store the physical address (frame base + offset within the page) in *paddr and
 * return 0. Return -1 if the page is not present, is swapped out, or the PFN reads as zero. */
int pm_decode(uint64_t entry, uint64_t vaddr, size_t page_size, uint64_t *paddr);

/* Exercise 1-1. Translate one virtual address of this process (use pagemap_read + pm_decode). */
int pm_virt_to_phys(pagemap_t *pm, const void *vaddr, uint64_t *paddr);

/* Exercise 1-2/1-3. A reverse map for one buffer: physical address -> virtual address. */
typedef struct revmap revmap_t; /* you define struct revmap in part1_pagemap.c */
revmap_t *revmap_build(pagemap_t *pm, void *buf, size_t len);   /* NULL on failure */
void *revmap_phys_to_virt(const revmap_t *rm, uint64_t paddr);  /* NULL if not in the buffer */
/* number of huge-page-sized, huge-aligned virtual chunks of the buffer whose pages are all
 * present and physically contiguous, starting at a huge-aligned physical address */
size_t revmap_count_huge(const revmap_t *rm);
void revmap_free(revmap_t *rm);

/* =========================================================================================
 * Part 2 - bank conflicts by timing (simulator)
 * ========================================================================================= */

/* Exercise 2-1. Access a and b alternately `rounds` times each and return a robust summary
 * (e.g. the median) of the per-access latency in cycles. */
double measure_pair(dsim_t *sim, uint64_t a, uint64_t b, int rounds);

/* Exercise 2-2. Find a latency threshold that separates "same bank, different row" pairs from
 * all other pairs. Use only timing (no knowledge of the mapping). */
double calibrate_threshold(dsim_t *sim, uint64_t seed);

/* Exercise 2-3. 1 if a and b are in the same bank but different rows, else 0. */
int same_bank_diff_row(dsim_t *sim, uint64_t a, uint64_t b, double threshold);

/* =========================================================================================
 * Part 3 - recovering the bank address functions (simulator)
 * ========================================================================================= */

/* Exercise 3-1 (easy). Exactly one of the three candidate sets is the real mapping; return its
 * index (0, 1 or 2). */
int pick_candidate(dsim_t *sim, const dsim_fnset_t cands[3], double threshold, uint64_t seed);

/* Exercise 3-2 (challenge). Recover the bank functions from timing alone. Write masks over the
 * physical-address bits that vary inside your buffer (bits 6 .. log2(buf_size)-1) to masks[]
 * and return how many you found. Any basis of the right GF(2) span is accepted. */
int recover_bank_fns(dsim_t *sim, double threshold, uint64_t seed, uint64_t *masks, int max_masks);

/* Exercise 3-3. Return the lowest physical-address bit that belongs to the row index. */
int find_row_lo(dsim_t *sim, double threshold, const uint64_t *masks, int nmasks, uint64_t seed);

/* =========================================================================================
 * Part 4 - hammering in the simulator
 * ========================================================================================= */

/* The address mapping, handed to you by the harness so that Part 4 does not depend on Part 3. */
typedef struct {
    int nfns;
    uint64_t fn[8];       /* bank bit i = parity(pa & fn[i]) */
    int pivot[8];         /* a bit that occurs in fn[i] and in no other fn (and below row_lo) */
    int row_lo;           /* row = (pa >> row_lo) & (rows_per_bank - 1) */
    unsigned rows_per_bank;
    uint64_t col_mask;    /* the 13 address bits that form the column (byte offset in the row) */
    uint64_t buf_base, buf_size;
    unsigned row_first, row_count; /* rows [row_first, row_first + row_count) of EVERY bank lie
                                      inside the buffer */
} dram_map_t;

/* Exercise 4-1. Physical address of byte `col` (0 .. 8191) of row `row` in bank `bank`.
 * Bank bit i of the result must equal bit i of `bank`. */
uint64_t map_compose(const dram_map_t *m, unsigned bank, unsigned row, unsigned col);

typedef enum { HAMMER_SINGLE = 1, HAMMER_DOUBLE = 2, HAMMER_MANY = 3 } hammer_kind_t;

typedef struct {
    hammer_kind_t kind;
    int sides;          /* HAMMER_MANY: number of aggressor rows (>= 2) */
    unsigned bank;
    unsigned row;       /* first aggressor row. SINGLE: aggressor `row` plus a far dummy row of
                           your choice in the same bank. DOUBLE: `row` and `row+2`. MANY: `row`,
                           `row+2`, ..., `row+2*(sides-1)`. */
    uint64_t pattern;   /* victims are filled with `pattern`, aggressors with ~pattern */
    double duration_ns; /* hammer for this much simulated time */
} hammer_cfg_t;

typedef struct {
    int victims;          /* rows checked: every non-aggressor row adjacent to an aggressor
                             (the dummy row's neighbours are not counted) */
    int victims_flipped;  /* victims with at least one flipped bit */
    int flips;            /* total flipped bits in the victims */
    int flips_1to0, flips_0to1;
} hammer_result_t;

/* Exercise 4-2. Fill, hammer, check. Return 0 on success, -1 on bad arguments. */
int hammer_once(dsim_t *sim, const dram_map_t *m, const hammer_cfg_t *cfg, hammer_result_t *res);

/* Exercise 4-3. `sim` has TRR enabled. Return the smallest number of aggressors (sides) for which
 * a many-sided pattern still produces bit flips, or -1 if you found none. */
int min_sides_under_trr(dsim_t *sim, const dram_map_t *m, uint64_t seed);

/* =========================================================================================
 * Part 5 - SECDED ECC: Hamming(39,32)
 * ========================================================================================= */
enum { ECC_OK = 0, ECC_CORRECTED = 1, ECC_UNCORRECTABLE = 2 };

/* Exercise 5-1. Encode 32 data bits into a 39-bit codeword (bits 0..38 of the result; all
 * higher bits zero). Any bit layout is fine as long as decode matches it. */
uint64_t secded_encode(uint32_t data);

/* Exercise 5-2. Decode a (possibly corrupted) codeword. Store the (corrected) data in *data and
 * return ECC_OK, ECC_CORRECTED (exactly one bit error fixed) or ECC_UNCORRECTABLE (an even
 * number of errors detected). Bits above bit 38 must be ignored. */
int secded_decode(uint64_t codeword, uint32_t *data);

/* =========================================================================================
 * Part 6 - mitigations in the memory controller (simulator)
 * ========================================================================================= */

/* Exercise 6-1. PARA: on every activation, refresh neighbour rows with probability p. */
double para_probability(const dsim_info_t *info);   /* choose p from the datasheet facts */
void *para_create(const dsim_info_t *info, double p, uint64_t seed);
void para_on_activate(void *state, dsim_t *sim, unsigned bank, unsigned row);
void para_destroy(void *state);

/* Exercise 6-2. Graphene: a per-bank Misra-Gries heavy-hitter table. */
typedef struct {
    unsigned threshold; /* refresh the neighbours whenever an estimated count reaches k*threshold */
    unsigned entries;   /* table entries per bank */
} graphene_params_t;
graphene_params_t graphene_params(const dsim_info_t *info);
void *graphene_create(const dsim_info_t *info, graphene_params_t p);
void graphene_on_activate(void *state, dsim_t *sim, unsigned bank, unsigned row);
void graphene_on_window(void *state, dsim_t *sim);
void graphene_destroy(void *state);

#endif
