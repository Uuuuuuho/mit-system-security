/* aslr_demo.c - what moves under `psp-lite --aslr` (also the loader's relocation test).
 *
 *   psp-lite build/examples/aslr_demo.elf            # loaded at the link address 0x80000000
 *   psp-lite --aslr build/examples/aslr_demo.elf     # loaded at a random 4 KiB-aligned base
 *
 * The whole image (code, data, bss, stack) slides by one random amount, so
 * relative distances are preserved: this coarse, per-image randomisation is
 * why a single disclosed pointer undoes ASLR (see L11).
 * The tables below hold absolute addresses; crt0's self-relocation fixes them.
 */
#include "psp.h"

static int counter;
static const char *const names[] = {"alpha", "beta", "gamma"};
static int twice(int x) { return x << 1; }
static int negate(int x) { return -x; }
static int (*const ops[])(int) = {twice, negate};

__attribute__((noinline)) static void show_frame(void) {
    int local = 0;
    printf("  &local (stack)   = %p\n", (void *)&local);
}

int main(void) {
    uint32_t base = psp_load_base();
    printf("load base (CSR 0xCC0)  = %p\n", (void *)base);
    printf("  &main  (text)    = %p   offset 0x%x\n", (void *)main, (uint32_t)(uintptr_t)main - base);
    printf("  &counter (bss)   = %p   offset 0x%x\n", (void *)&counter, (uint32_t)(uintptr_t)&counter - base);
    show_frame();
    counter = ops[1](-5) + ops[0](1);
    printf("relocated tables work: names[2]=%s ops -> %d\n", names[2], counter);
    return 0;
}
