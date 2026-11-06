/* bench.c - a simple loop to measure simulator speed:  psp-lite --stats build/examples/bench.elf */
#include "psp.h"

int main(void) {
    uint32_t x = 1, acc = 0;
    for (uint32_t i = 0; i < 20000000u; i++) { /* ~6 instructions per iteration */
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        acc ^= x;
    }
    printf("bench done: %08x\n", acc);
    return 0;
}
