/* Part 2 - Flush+Reload CTF.                                     (Exercise 2-1)
 *
 * The victim runs on the other core and keeps loading  shared + secret * FR_STRIDE  for a secret
 * slot in [0, FR_SLOTS). Now and then it also touches a random decoy slot. `shared` is mapped
 * read-only into both of you (think: a shared library page). Find the secret slot.
 *
 * Recipe: flush every slot, let the victim run (plat_wait), reload every slot with plat_probe and
 * count which slots come back fast (<= cal->llc_miss: someone brought them into the cache).
 * Repeat and vote. Watch out for the stride prefetcher (README, Part 2 hints).
 */
#include <string.h>

#include "lab.h"

int fr_attack(const uint8_t *shared, const cal_t *cal) {
    (void)shared;
    (void)cal;
    /* TODO (Exercise 2-1) */
    return -1;
}
