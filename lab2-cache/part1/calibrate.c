/* Part 1 - cache latency histograms and thresholds.            (Exercises 1-1 and 1-2)
 *
 * Goal: for each level of the hierarchy (L1, L2, LLC, DRAM), put a line exactly there and time one
 * load of it with plat_probe(). Then pick two thresholds that later parts use:
 *     l2_miss  : probe > l2_miss   means "not in L1/L2"  (Prime+Probe on the L2, Parts 3-5)
 *     llc_miss : probe > llc_miss  means "from DRAM"     (Flush+Reload, Part 2)
 *
 * Useful facts (see README "Address bits" and plat_geom()):
 *   - L1 set index = PA bits 6..(6+log2(l1_sets)), L2 set index = PA bits 6..(6+log2(l2_sets)).
 *   - In a 2 MiB huge page, VA bits 0..20 equal PA bits 0..20, so lines that are
 *     l2_sets*line bytes apart are in the same L2 set; lines l1_sets*line apart share an L1 set.
 *   - Every level is inclusive: evicting a line from L2 also evicts it from L1.
 *   - Only use plat_probe() for the measurement itself; prepare the cache state with
 *     plat_access() / plat_flush().
 */
#include <stdlib.h>
#include <string.h>

#include "lab.h"

void measure_level(int level, uint64_t *samples, int n) {
    /* TODO (Exercise 1-1):
     *   allocate a buffer once (plat_alloc(bytes, 1) for huge pages - how many bytes do you need to
     *   hold more than l2_ways lines of the same L2 set?), then for each sample:
     *     LVL_L1  : touch x, then time it
     *     LVL_L2  : touch x, evict it from L1 only (lines that share x's L1 set but NOT its L2 set)
     *     LVL_LLC : touch x, evict it from L2 (more than l2_ways lines congruent with x in L2)
     *     LVL_DRAM: touch x, flush it
     *   Vary x between samples (why does that matter? hint: prefetchers). */
    (void)level;
    for (int i = 0; i < n; i++) samples[i] = 0;
}

cal_t calibrate(void) {
    cal_t c;
    memset(&c, 0, sizeof c);
    /* TODO (Exercise 1-2): call measure_level() for every level, fill c.med[LVL_*] with medians and
     * choose c.l2_miss and c.llc_miss so that they separate the distributions with the largest
     * margin (the sim grader checks them against the model's true distributions). */
    return c;
}
