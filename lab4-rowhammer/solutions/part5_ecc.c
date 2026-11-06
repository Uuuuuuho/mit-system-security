/* part5_ecc.c - REFERENCE SOLUTION for Part 5. SPOILER: try the lab yourself first.
 *
 * Extended Hamming code, 32 data bits -> 39-bit codeword (SECDED).
 * Codeword bit k holds Hamming position k:
 *   position 0                  overall parity (makes the whole codeword even)
 *   positions 1,2,4,8,16,32     check bits; check bit 2^i covers every position with bit i set
 *   the other 32 positions      data bits d0..d31, in increasing order (3,5,6,7,9,...,38)
 * Syndrome = XOR of the positions of all set bits in 1..38 (0 for a valid codeword).
 *
 * Exercise 5-1  secded_encode
 * Exercise 5-2  secded_decode
 */
#include "../include/lab4.h"

#define NPOS 39

static int data_pos[32];
static int pos_init;

static void init_positions(void) {
    int d = 0;
    for (int p = 1; p < NPOS; p++)
        if (p & (p - 1)) data_pos[d++] = p; /* not a power of two */
    pos_init = 1;
}

uint64_t secded_encode(uint32_t data) {
    if (!pos_init) init_positions();
    uint64_t cw = 0;
    unsigned syn = 0;
    for (int i = 0; i < 32; i++)
        if ((data >> i) & 1) { cw |= 1ull << data_pos[i]; syn ^= (unsigned)data_pos[i]; }
    /* choose the check bits so that the syndrome becomes zero */
    for (int i = 0; i < 6; i++)
        if ((syn >> i) & 1) cw |= 1ull << (1u << i);
    if (__builtin_parityll(cw)) cw |= 1ull; /* overall parity */
    return cw;
}

int secded_decode(uint64_t codeword, uint32_t *data) {
    if (!pos_init) init_positions();
    uint64_t cw = codeword & ((1ull << NPOS) - 1);
    unsigned syn = 0;
    for (int p = 1; p < NPOS; p++)
        if ((cw >> p) & 1) syn ^= (unsigned)p;
    int odd = __builtin_parityll(cw);
    int status = ECC_OK;
    if (odd) {
        /* an odd number of errors: assume one */
        if (syn == 0) cw ^= 1ull;                    /* the overall parity bit itself */
        else if (syn < NPOS) cw ^= 1ull << syn;      /* the bit the syndrome points at */
        else status = ECC_UNCORRECTABLE;             /* points outside the codeword: >= 3 errors */
        if (status == ECC_OK) status = ECC_CORRECTED;
    } else if (syn != 0) {
        status = ECC_UNCORRECTABLE;                  /* even and non-zero syndrome: double error */
    }
    uint32_t d = 0;
    for (int i = 0; i < 32; i++) d |= (uint32_t)((cw >> data_pos[i]) & 1) << i;
    *data = d;
    return status;
}
