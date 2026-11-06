/* part5_ecc.c - Part 5: SECDED Hamming(39,32).                       (YOUR CODE GOES HERE)
 *
 * Exercise 5-1  secded_encode
 * Exercise 5-2  secded_decode
 *
 * Build and test:  make && ./build/part5 --seed 1
 *
 * Plan (one classic layout - any layout works if encode and decode agree):
 *   number the codeword bits 0..38; bit 0 = overall parity; bits 1, 2, 4, 8, 16, 32 = check bits;
 *   the remaining 32 positions hold the data bits. Check bit 2^i makes the XOR of all positions
 *   whose index has bit i set equal to 0. Then the "syndrome" (XOR of the indices of all set bits
 *   among 1..38) is 0 for a valid codeword and equals the index of a single flipped bit.
 *   The overall parity tells "odd number of errors" (correct one) from "even" (detect only).
 */
#include "../include/lab4.h"

/* ---------------------------------------------------------------- Exercise 5-1 */

uint64_t secded_encode(uint32_t data) {
    /* TODO */
    (void)data;
    return 0;
}

/* ---------------------------------------------------------------- Exercise 5-2 */

int secded_decode(uint64_t codeword, uint32_t *data) {
    /* TODO: ignore bits above 38; compute syndrome and overall parity; correct or detect;
     * extract the data bits. Return ECC_OK, ECC_CORRECTED or ECC_UNCORRECTABLE. */
    (void)codeword;
    *data = 0;
    return ECC_UNCORRECTABLE;
}
