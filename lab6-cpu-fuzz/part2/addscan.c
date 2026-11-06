/* addscan.c - Part 2: exhaustive differential test of the ADD instruction (STARTER).
 *
 * Exercise 2-1: for every a in [LO, HI) and b in [LO, HI) (a outer, b inner,
 * ascending) compare the CPU's ADD (psp_hw_add() in psp.h) with a reference
 * that does not use the adder.  Careful: your LOOP CONTROL (a++, b++, count++)
 * also runs on the faulty adder.  Make it robust (advance counters without ADD,
 * and cross-check the number of iterations at the end).
 *
 * Required output format (parsed by the grader):
 *   count=<number of mismatching pairs>
 *   first a=0x........ b=0x........ got=0x........ want=0x........     (only if count > 0)
 *   pattern nibble_a=<a&15> nibble_b=<b&15> bit=<index of the flipped bit>   (only if count > 0)
 *   iterations=<pairs tested>
 */
#include "psp.h"

#define LO 0x100u
#define HI 0x500u

int main(void) {
    uint32_t count = 0, iters = 0;
    printf("addscan: a,b in [0x%x, 0x%x)\n", LO, HI);
    /* TODO (Exercise 2-1) */
    printf("count=%u\n", count);
    printf("iterations=%u\n", iters);
    return 0;
}
