/* collector.c - sweep-counting cache-occupancy collector (Part 2).  REFERENCE SOLUTION.
 *
 * Idea (Shusterman et al. 2019, Cook et al. 2022): repeatedly touch one byte in every cache line
 * of an LLC-sized buffer and count how many complete sweeps fit in each window of P ms. When
 * another program uses the cache or memory system, our lines get evicted (or our accesses get
 * slower), fewer sweeps complete, and the count drops. When we are descheduled or interrupted,
 * the count drops too (Part 3 asks which effect dominates).
 */
#define COLLECTOR_NAME "sweep"
#include "collector_frame.h"

static uint64_t measure_window(const collector_ctx *c, uint64_t deadline_ns) {
    uint64_t sweeps = 0;
    const volatile uint8_t *b = c->buf;
    /* Check the clock only between sweeps (like the JavaScript attack, which cannot afford a
     * timer read per access). A sweep that straddles the deadline still counts. */
    while (fp_now_ns() < deadline_ns) {
        for (size_t off = 0; off < c->bytes; off += c->line) (void)b[off];
        sweeps++;
    }
    return sweeps;
}
