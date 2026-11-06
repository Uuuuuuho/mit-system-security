/* Part 5 - covert channel SENDER.                            (Exercise 5-1)
 *
 * You share a core (L1 + L2) with the receiver, but no writable memory, and plat_flush() is
 * disabled. Signal bits by filling (or not filling) agreed-upon L2 sets during agreed-upon time
 * slots; the receiver Prime+Probes them. Your protocol must carry messages of ANY length:
 * add framing (start marker, length, checksum...). Whatever you need from Part 3 is available.
 *
 * Rules: talk to the receiver only through the cache. Do not use files, pipes, shared memory,
 * signals or environment variables (the grader checks the symbols your object file uses).
 */
#include <stdio.h>
#include <string.h>

#include "channel.h"

void sender_main(const char *msg, const cal_t *cal) {
    (void)msg;
    (void)cal;
    /* TODO (Exercise 5-1) */
}
