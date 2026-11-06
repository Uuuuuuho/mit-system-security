/* Part 4 - the victim's table lookup (the victim calls these; you fix the leak). (Exercise 4-3)
 *
 * victim_lookup() is the original code: which table it reads depends on the key bit, so the set
 * of cache lines it touches leaks the bit. Write victim_lookup_ct(): it must return exactly the same
 * value, but the sequence of cache lines it touches (through plat_access/plat_read8) must be the
 * same for bit=0 and bit=1. Check it with  ./bin/part4 --ct-check  and then run your Part 4 attack
 * against the fixed victim with  ./bin/part4 --defend.
 */
#include "lab.h"

/* Given: the leaky original. Do not change it. */
uint8_t victim_lookup(int bit, unsigned i, const uint8_t *t_zero, const uint8_t *t_one) {
    const uint8_t *t = bit ? t_one : t_zero;
    return plat_read8(t + (size_t)i * 64);
}

uint8_t victim_lookup_ct(int bit, unsigned i, const uint8_t *t_zero, const uint8_t *t_one) {
    /* TODO (Exercise 4-3): no secret-dependent addresses, no secret-dependent branches. */
    (void)bit;
    (void)i;
    (void)t_zero;
    (void)t_one;
    return 0;
}
