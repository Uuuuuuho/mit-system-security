/* collector.c - sweep-counting cache-occupancy collector (Part 2, Exercise 2-1).
 *
 * Idea (Shusterman et al. 2019, Cook et al. 2022): repeatedly touch one byte in every cache line
 * of an LLC-sized buffer and count how many complete sweeps fit in each window of P ms. When
 * another program uses the cache or memory system, our lines get evicted (or our accesses get
 * slower), fewer sweeps complete, and the count drops.
 *
 * Everything except the measurement (argument parsing, buffer allocation, warm-up, window
 * deadlines, output format) lives in common/collector_frame.h. Read it first.
 */
#define COLLECTOR_NAME "sweep"
#include "collector_frame.h"

/* Exercise 2-1: implement the sweep loop.
 *
 * Keep sweeping the buffer until fp_now_ns() >= deadline_ns and return the number of COMPLETE
 * sweeps. One sweep = read one byte from every cache line of the buffer:
 *     c->buf[0], c->buf[c->line], c->buf[2 * c->line], ...   (c->bytes bytes in total)
 *
 * Requirements / hints:
 *  - Read the clock only between sweeps, not per access (the JavaScript attack cannot afford a
 *    timer read per access either; also fp_now_ns() costs ~20 ns).
 *  - c->buf is `volatile`, so the compiler will not delete your loads. Do not cast it away.
 *  - A sweep that is still running when the deadline passes may simply be counted.
 *  - Do not return early: the frame relies on you returning only once the deadline has passed.
 */
static uint64_t measure_window(const collector_ctx *c, uint64_t deadline_ns) {
    uint64_t sweeps = 0;
    (void)c;
    while (fp_now_ns() < deadline_ns) {
        /* TODO(Exercise 2-1): one complete sweep over the buffer, then count it. */
    }
    return sweeps;
}
