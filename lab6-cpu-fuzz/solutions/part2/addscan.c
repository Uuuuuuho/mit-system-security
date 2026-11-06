/* addscan.c - Part 2: exhaustive differential test of the ADD instruction
 * (REFERENCE SOLUTION - spoiler).
 *
 * For every a in [LO, HI) and b in [LO, HI) (a outer, b inner, ascending) we
 * compare the CPU's ADD (psp_hw_add) with an adder built from AND/XOR/shift.
 * Loop control does not trust the adder either: counters advance with
 * soft_inc(), and the number of iterations is cross-checked at the end, so a
 * faulty adder cannot silently skip or repeat part of the range.
 *
 * Output format (parsed by the grader):
 *   count=<number of mismatching pairs>
 *   first a=0x........ b=0x........ got=0x........ want=0x........
 *   pattern nibble_a=<a&15> nibble_b=<b&15> bit=<index of the flipped bit>
 *   iterations=<pairs tested>
 */
#include "psp.h"

#define LO 0x100u
#define HI 0x500u

static uint32_t soft_add(uint32_t a, uint32_t b) {
    while (b) {
        uint32_t carry = a & b;
        a ^= b;
        b = carry << 1;
    }
    return a;
}

/* x + 1 without the adder: flip the trailing ones and the first zero */
static uint32_t soft_inc(uint32_t x) {
    uint32_t m = 1;
    while (x & m) {
        x ^= m;
        m <<= 1;
    }
    return x ^ m;
}

int main(void) {
    uint32_t count = 0, iters = 0;
    uint32_t fa = 0, fb = 0, fgot = 0, fwant = 0;
    printf("addscan: a,b in [0x%x, 0x%x)\n", LO, HI);
    for (uint32_t a = LO; a != HI; a = soft_inc(a)) {
        for (uint32_t b = LO; b != HI; b = soft_inc(b)) {
            uint32_t got = psp_hw_add(a, b), want = soft_add(a, b);
            if (got != want) {
                if (count == 0) {
                    fa = a;
                    fb = b;
                    fgot = got;
                    fwant = want;
                }
                count = soft_inc(count);
            }
            iters = soft_inc(iters);
        }
    }
    printf("count=%u\n", count);
    if (count) {
        uint32_t diff = fgot ^ fwant, bit = 0;
        while (!(diff & 1u)) {
            diff >>= 1;
            bit = soft_inc(bit);
        }
        printf("first a=0x%08x b=0x%08x got=0x%08x want=0x%08x\n", fa, fb, fgot, fwant);
        printf("pattern nibble_a=%u nibble_b=%u bit=%u\n", fa & 15u, fb & 15u, bit);
    }
    printf("iterations=%u\n", iters);
    if (iters != (HI - LO) * (HI - LO)) printf("WARNING: loop control was disturbed\n");
    return 0;
}
