/* ss_demo.c - a corrupted return address, with and without the shadow stack.
 *
 *   psp-lite build/examples/ss_demo.elf                  -> the bad return is only noticed later, as a
 *                                                           fetch fault at a garbage address (exit 64+cause)
 *   psp-lite --shadow-stack build/examples/ss_demo.elf   -> with the stub hook: same late crash
 *   (simulator built with solutions/shadow_stack.c)      -> caught AT the return: trap mcause=18, exit 82
 *
 * Built with -fno-omit-frame-pointer so the saved return address of vuln() is
 * at -4(s0), as in the RISC-V frame layout.  Before the attack the program
 * does plenty of legitimate calls (recursion, function pointers, tail calls)
 * so a correct shadow stack must not raise false alarms.
 */
#include "psp.h"

__attribute__((noinline)) static uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
__attribute__((noinline)) static uint32_t sq(uint32_t x) { return x * x; }
__attribute__((noinline)) static uint32_t tail(uint32_t x) { return sq(x); } /* compiled as a tail call */

__attribute__((noinline)) static void vuln(void) {
    volatile char buf[16];
    uint32_t *fp = __builtin_frame_address(0); /* s0: top of this frame */
    for (int i = 0; i < 16; i++) buf[i] = 'A';
    puts("vuln: corrupting my saved return address");
    fp[-1] = 0x00000F00u; /* garbage, as a linear overflow of buf reaching the saved ra would leave */
}

int main(void) {
    uint32_t (*fp)(uint32_t) = sq;
    printf("fib(15)=%u sq(12)=%u tail(9)=%u\n", fib(15), fp(12), tail(9));
    vuln();
    puts("returned normally (corruption had no effect)");
    return 0;
}
