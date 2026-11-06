/* Part 3 - Prime+Probe CTF.                                (Exercises 3-2 and 3-3)
 *
 * Your SMT sibling (same core: shares your L1 and L2) hammers `victim_lines` lines that all map to
 * one secret L2 set: 16 lines all the time in easy mode (./bin/part3 --mode easy); in hard mode only
 * 4 lines and only in bursts (~25% of the time, period ~100k ticks in sim). It also touches
 * unrelated memory, and the "OS" adds background noise. Return the secret set index.
 *
 * Plan: for every set s: prime an eviction set for s, wait, probe, count misses. The victim's set
 * is the one that keeps losing lines. Scan everything quickly, then re-measure the best candidates
 * longer. Remember that plat_wait() is what lets the victim run between your prime and probe.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lab.h"

int pp_find_set(const cal_t *cal, int victim_lines) {
    (void)cal;
    (void)victim_lines;
    /* TODO (Exercises 3-2, 3-3) */
    return -1;
}
