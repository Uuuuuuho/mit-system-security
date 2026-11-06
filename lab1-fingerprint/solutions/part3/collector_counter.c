/* collector_counter.c - the sweep collector with every memory access removed (Part 3).
 * REFERENCE SOLUTION.
 *
 * Same windows, same output format, but the work unit is a block of empty loop iterations that
 * lives entirely in registers. The only memory this loop touches is whatever clock_gettime()
 * reads from the vDSO data page, which the JavaScript attacks also cannot avoid. If this still
 * fingerprints sites, the signal cannot be (only) cache occupancy.
 */
#define COLLECTOR_NAME "counter"
#include "collector_frame.h"

#define BLOCK 1000 /* iterations between clock reads */

static uint64_t measure_window(const collector_ctx *c, uint64_t deadline_ns) {
    (void)c; /* the buffer is deliberately unused */
    uint64_t blocks = 0;
    while (fp_now_ns() < deadline_ns) {
        /* An empty volatile asm cannot be deleted or merged, and has no "memory" clobber, so the
         * compiler keeps the loop in registers without emitting loads or stores. */
        for (int i = 0; i < BLOCK; i++) __asm__ volatile("");
        blocks++;
    }
    return blocks;
}
