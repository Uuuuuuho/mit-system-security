/* news: a heavy front page. Big streaming burst while the page loads (images, layout), then
 * rotating ad slots refresh every ~250 ms with a short streaming + compute burst. */
#include "site.h"

int main(int argc, char **argv) {
    site_init(argc, argv, 48, 0);
    stream_ms(jit(280, 0.2), S.big_bytes);  /* page load */
    compute_ms(jit(40, 0.3));               /* script evaluation */
    double next = t_ms() + jit(120, 0.3);
    while (running()) {
        idle_until(next);
        stream_ms(jit(25, 0.25), 24 * MiB); /* ad refresh */
        compute_ms(jit(8, 0.3));
        next += jit(250, 0.15);
    }
    return site_exit();
}
