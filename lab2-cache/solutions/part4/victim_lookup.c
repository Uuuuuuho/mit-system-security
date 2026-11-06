/* Part 4 - REFERENCE SOLUTION (spoilers): the leaky lookup and a constant-trace version. */
#include "lab.h"

/* Given: the leaky original. */
uint8_t victim_lookup(int bit, unsigned i, const uint8_t *t_zero, const uint8_t *t_one) {
    const uint8_t *t = bit ? t_one : t_zero;
    return plat_read8(t + (size_t)i * 64);
}

/* Read BOTH tables, always in the same order, and select the wanted byte with a mask instead of a
 * branch or a secret-dependent address. Cost: one extra load (and one extra line in the cache). */
uint8_t victim_lookup_ct(int bit, unsigned i, const uint8_t *t_zero, const uint8_t *t_one) {
    uint8_t a = plat_read8(t_zero + (size_t)i * 64);
    uint8_t b = plat_read8(t_one + (size_t)i * 64);
    uint8_t mask = (uint8_t)(0u - (unsigned)(bit & 1)); /* 0x00 for bit 0, 0xff for bit 1 */
    return (uint8_t)((a & (uint8_t)~mask) | (b & mask));
}
