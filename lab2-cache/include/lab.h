/* lab.h - interfaces between the lab drivers and YOUR code. Do not edit; read it.
 *
 * Every function below marked "(you)" lives in a file under partN/ that you complete.
 * Later parts reuse earlier ones: Parts 2-5 call your calibrate() from Part 1, and Parts 4-5 may
 * use your Part 3 eviction-set helpers.
 */
#ifndef LAB_H
#define LAB_H
#include <stddef.h>
#include <stdint.h>

#include "plat.h"

/* ================================================================ Part 1: timing */
enum { LVL_L1 = 1, LVL_L2 = 2, LVL_LLC = 3, LVL_DRAM = 4 };

typedef struct {
    uint64_t med[5];  /* med[LVL_x] = median probe latency of a load served by level x (ticks)      */
    uint64_t l2_miss; /* plat_probe() >  l2_miss   =>  the line was in neither L1 nor L2 (Parts 3-5) */
    uint64_t llc_miss;/* plat_probe() >  llc_miss  =>  the line came from DRAM           (Part 2)    */
} cal_t;

/* (you) Fill samples[0..n) with plat_probe() latencies of loads that are served by `level`. */
void measure_level(int level, uint64_t *samples, int n);

/* (you) Measure all levels (using measure_level) and derive med[] and the two thresholds. */
cal_t calibrate(void);

/* ================================================================ Part 2: Flush+Reload */
/* The victim (on the other core) keeps touching  shared + secret * FR_STRIDE,  secret in [0, FR_SLOTS),
 * plus a few decoy slots now and then. `shared` is mapped read-only into both processes. */
#define FR_SLOTS 1024
#define FR_STRIDE ((size_t)(plat_is_sim() ? 128 : 4096)) /* native: one slot per page */

/* (you) Return the victim's secret slot. */
int fr_attack(const uint8_t *shared, const cal_t *cal);

/* ================================================================ Part 3: Prime+Probe */
/* A pool of memory used to build eviction sets. */
typedef struct {
    uint8_t *base;
    size_t bytes;
    int huge; /* 1 if backed by 2 MiB pages (then VA bits 0..20 equal PA bits 0..20) */
} pool_t;

/* (you) Allocate the pool (ask for huge pages). Returns 0 on success. */
int pool_init(pool_t *pool, size_t bytes);

/* (you) Store in lines[0..n) addresses from the pool that all map to L2 set `set`.
 * Returns how many it found (n on success). */
int evset_build(const pool_t *pool, unsigned set, int n, uint8_t **lines);

/* (you) Prime: bring lines[0..n) into the cache.
 *       Probe: access them again, timing each; return how many took longer than cal->l2_miss. */
void pp_prime(uint8_t *const *lines, int n);
int pp_probe(uint8_t *const *lines, int n, const cal_t *cal);

/* (you) The victim (SMT sibling, shares your L1 and L2) hammers `victim_lines` lines that all live in
 * ONE secret L2 set. Return that set's index in [0, plat_geom()->l2_sets). */
int pp_find_set(const cal_t *cal, int victim_lines);

/* ================================================================ Part 4: key-dependent paths */
/* Public "binary layout" of the Part 4 victim (think: you disassembled it).
 * The victim keeps two lookup tables, each in its own 4 KiB page (small pages: you do NOT know the
 * physical page, only the offset inside it):
 *   table_zero at page offset KEY_TABLE_ZERO_OFF, table_one at KEY_TABLE_ONE_OFF,
 *   each KEY_TABLE_LINES cache lines long.
 * It loops forever over its key:  [KEY_GAP_SLOTS idle slots][bit 0][bit 1]...[bit n-1]  where every
 * slot lasts KEY_SLOT_TICKS. During a bit slot it repeatedly calls victim_lookup(bit, i, ...) for
 * i = 0..KEY_TABLE_LINES-1. Idle slots do unrelated work. */
#define KEY_TABLE_ZERO_OFF 0x340
#define KEY_TABLE_ONE_OFF 0xA80
#define KEY_TABLE_LINES 4
#define KEY_SLOT_TICKS ((uint64_t)(plat_is_sim() ? 60000 : 4000000)) /* native: ~2 ms at 2 GHz */
#define KEY_GAP_SLOTS 2

/* The victim's lookup: return entry i of the table selected by `bit` (entries are 64 bytes apart).
 * victim_lookup is the original, leaky version (given). */
uint8_t victim_lookup(int bit, unsigned i, const uint8_t *t_zero, const uint8_t *t_one);
/* (you, Exercise 4-3) Same result, but the sequence of cache lines touched must not depend on bit. */
uint8_t victim_lookup_ct(int bit, unsigned i, const uint8_t *t_zero, const uint8_t *t_one);

/* (you) Recover the victim's key: write key_bits values (0/1) to bits[]. Return 0 on success. */
int leak_key(const cal_t *cal, int key_bits, uint8_t *bits);

/* ================================================================ Part 5: covert channel */
/* Sender and receiver are separate parties on the same core (they share L1 and L2). They share NO
 * writable memory, and plat_flush() is disabled. Both get a copy of the calibration that the driver
 * ran (with your calibrate()) before they started. */

/* (you) Transmit msg (a NUL-terminated string of any length) and return. */
void sender_main(const char *msg, const cal_t *cal);

/* (you) Receive one message into out (NUL-terminate it). Return its length, or -1 on failure. */
int receiver_main(char *out, size_t cap, const cal_t *cal);

#endif
