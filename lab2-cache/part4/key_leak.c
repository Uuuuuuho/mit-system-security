/* Part 4 - leak a key from key-dependent code paths.        (Exercises 4-1 and 4-2)
 *
 * Read the victim's "binary layout" in include/lab.h (KEY_*). For every key bit the victim reads
 * table_one (bit = 1) or table_zero (bit = 0). You know each table's page OFFSET but not its
 * physical page, so each table can live in any of  l2_sets / (page/line)  L2 sets.
 *
 *   Exercise 4-1: find which L2 set(s) each table really uses (watch every candidate for a full
 *                 key period; the right one is busy some of the time).
 *   Exercise 4-2: watch both sets over time and turn the activity into bits. The key repeats every
 *                 (KEY_GAP_SLOTS + key_bits) * KEY_SLOT_TICKS ticks; the idle gap marks its start.
 *
 * Reuse your Part 3 helpers (pool_init / evset_build / pp_prime / pp_probe).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lab.h"

int leak_key(const cal_t *cal, int key_bits, uint8_t *bits) {
    (void)cal;
    for (int i = 0; i < key_bits; i++) bits[i] = 0xff; /* "unknown" */
    /* TODO (Exercises 4-1, 4-2) */
    return -1;
}
