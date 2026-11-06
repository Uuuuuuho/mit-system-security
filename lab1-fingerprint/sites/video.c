/* video: a streaming video page. Buffering burst, then one frame every ~33 ms (30 fps): decode
 * (stream through a frame-sized buffer) + post-processing (compute), idle until the next vsync. */
#include "site.h"

int main(int argc, char **argv) {
    site_init(argc, argv, 16, 0);
    stream_ms(jit(150, 0.2), S.big_bytes);  /* buffering */
    double period = jit(33.3, 0.05);        /* frame rate varies a little per run */
    double next = t_ms();
    while (running()) {
        stream_ms(jit(7, 0.2), 8 * MiB);    /* decode frame */
        compute_ms(jit(3, 0.3));            /* colour conversion */
        next += period;
        idle_until(next);
    }
    return site_exit();
}
