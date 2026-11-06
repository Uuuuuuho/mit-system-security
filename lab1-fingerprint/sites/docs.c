/* docs: a static documentation page. Short load, then essentially idle except a cursor-blink
 * timer every ~500 ms. The "quiet" baseline that other sites must be distinguished from. */
#include "site.h"

int main(int argc, char **argv) {
    site_init(argc, argv, 8, 0);
    stream_ms(jit(60, 0.25), S.big_bytes);  /* load */
    compute_ms(jit(20, 0.3));               /* layout */
    double next = t_ms() + jit(300, 0.3);
    while (running()) {
        idle_until(next);
        compute_ms(jit(2, 0.3));            /* blink */
        next += jit(500, 0.1);
    }
    return site_exit();
}
