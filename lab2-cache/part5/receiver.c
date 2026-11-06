/* Part 5 - covert channel RECEIVER.                          (Exercise 5-1)
 *
 * Prime+Probe the agreed sets, turn the miss counts of each time slot into symbols, find the start
 * of a message, decode it, verify it, and write it into out (NUL-terminated). Return its length,
 * or -1 if you give up. Give up eventually: the grader has a time limit.
 */
#include <stdio.h>
#include <string.h>

#include "channel.h"

int receiver_main(char *out, size_t cap, const cal_t *cal) {
    (void)cal;
    /* TODO (Exercise 5-1) */
    if (cap) out[0] = 0;
    return -1;
}
