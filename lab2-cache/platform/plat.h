/* plat.h - the ONLY hardware interface your attack code may use in lab2-cache.
 *
 * The same calls are implemented by two backends, selected at run time with SHD_BACKEND:
 *
 *   sim     (default) A deterministic model of a 2-core machine: private L1D + L2 per core, a shared
 *           inclusive LLC, 64-byte lines, physical pages, prefetchers, timer noise and background
 *           "OS" activity. Every party (your attacker, the victim, the covert-channel sender and
 *           receiver) runs as a cooperative coroutine in *virtual time*. Every plat_* call charges
 *           virtual cycles to the calling party; plat_wait() lets the other parties run.
 *
 *   native  Real memory and real timing on the machine you are sitting at (via common/shd.h).
 *           Victims are real processes (fork) pinned to CPUs. Works best on x86_64 Linux.
 *
 * Units: all times (plat_probe, plat_now, plat_wait) are in "ticks" of the backend's timer:
 * simulated cycles in sim; TSC ticks (x86_64) or generic-timer ticks (aarch64) in native.
 *
 * Addresses are ordinary pointers. You may read/write memory you allocated with plain C (e.g. to
 * build linked lists); the cache model only sees accesses made through plat_access/plat_probe/
 * plat_read8. Plain C accesses are "free" in sim and real in native.
 */
#ifndef PLAT_H
#define PLAT_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int line;               /* cache line size (bytes)                                       */
    int l1_sets, l1_ways;   /* private L1 data cache                                          */
    int l2_sets, l2_ways;   /* private L2 (indexed by physical address bits 6 .. 6+log2(sets)) */
    int llc_sets, llc_ways; /* shared last-level cache (native: approximate, LLCs are sliced)  */
    size_t page;            /* small page size: bits below log2(page) are identical in VA/PA  */
    size_t huge;            /* huge page size                                                  */
} plat_geom_t;

/* Name of the active backend: "sim" or "native". */
const char *plat_backend(void);
int plat_is_sim(void);

/* Cache/page geometry of the (simulated or real) machine. */
const plat_geom_t *plat_geom(void);

/* Allocate `bytes` of page-aligned, pre-faulted memory private to the calling party (contents
 * unspecified; native fills it with non-zero bytes so no page is backed by the shared zero page).
 * huge=1 asks for 2 MiB pages (sim: always granted unless SHD_SIM_NOHUGE=1; native: best effort
 * via hugetlbfs or transparent huge pages). With huge pages, VA bits [0,21) equal PA bits [0,21). */
void *plat_alloc(size_t bytes, int huge);

/* 1 if p lies in memory that is really backed by huge pages (so VA bits 0..20 == PA bits 0..20). */
int plat_is_huge(const void *p);

/* One load of the byte at p (no timing). */
void plat_access(const void *p);

/* One load of the byte at p; returns its value (useful when the value matters, e.g. Part 4). */
uint8_t plat_read8(const void *p);

/* One timed load: returns the latency in ticks, including the timer's own overhead. */
uint64_t plat_probe(const void *p);

/* Evict the line containing p from every cache level of every core (clflush / dc civac).
 * Disabled in Part 5 (calling it there aborts the program). */
void plat_flush(const void *p);

/* Memory fence (orders the accesses before and after it). */
void plat_fence(void);

/* Current time in ticks. All parties share one time base (like the TSC). */
uint64_t plat_now(void);

/* Spend `ticks` doing nothing useful. This is how you let the other parties run in sim.
 * (native: spins; yields the CPU when a victim shares it without SMT.) */
void plat_wait(uint64_t ticks);

/* Deterministic pseudo-random numbers (seeded from SEED / --seed). */
uint64_t plat_rand(void);

#endif
