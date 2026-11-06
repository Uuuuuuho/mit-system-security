/* search: the user types a query (a small burst per keystroke for suggestions), presses enter,
 * the result page loads in one heavy burst, then the page goes quiet. */
#include "site.h"

int main(int argc, char **argv) {
    site_init(argc, argv, 40, 0);
    stream_ms(jit(40, 0.3), 8 * MiB);       /* tiny landing page */
    double typing_end = S.duration_s * 1000.0 * urange(0.35, 0.5);
    while (running() && t_ms() < typing_end) {
        idle_ms(urange(70, 160));           /* time between keystrokes */
        stream_ms(jit(12, 0.3), 4 * MiB);   /* suggestions */
        compute_ms(jit(4, 0.3));
    }
    if (running()) {
        stream_ms(jit(220, 0.2), S.big_bytes); /* results page */
        compute_ms(jit(40, 0.3));
    }
    while (running()) idle_ms(20);
    return site_exit();
}
