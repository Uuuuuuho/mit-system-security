/* collector_counter.c - Part 3, Exercise 3-1: the collector WITHOUT memory accesses.
 *
 * Start from your Part 2 sweep loop and remove every memory access: same windows, same output
 * format, but the unit of work is now a block of empty loop iterations that lives entirely in
 * registers. If this collector still fingerprints the sites, the signal cannot be (only) cache
 * occupancy. (The only memory left is what clock_gettime() reads from the vDSO page; the
 * JavaScript attack cannot avoid its timer either.)
 */
#define COLLECTOR_NAME "counter"
#include "collector_frame.h"

#define BLOCK 1000 /* iterations between clock reads */

/* Exercise 3-1: until fp_now_ns() >= deadline_ns, run blocks of BLOCK empty iterations and
 * return how many blocks completed.
 *
 * Hints:
 *  - Do not touch c->buf (the grader checks that your counts do not depend on -b).
 *  - An empty loop is deleted by the optimizer. `__asm__ volatile("");` inside the loop body
 *    cannot be deleted and, having no "memory" clobber, does not force loads or stores.
 *    Check with `objdump -d build/collector_counter | less` that the loop has no memory operands.
 */
static uint64_t measure_window(const collector_ctx *c, uint64_t deadline_ns) {
    uint64_t blocks = 0;
    (void)c;
    while (fp_now_ns() < deadline_ns) {
        /* TODO(Exercise 3-1): one block of BLOCK register-only iterations, then count it. */
    }
    return blocks;
}
