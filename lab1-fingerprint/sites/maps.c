/* maps: a slippy map. Tiles arrive in irregular bursts of random access over a large tile cache;
 * the bursts get longer as the user zooms in over the course of the visit. */
#include "site.h"

int main(int argc, char **argv) {
    site_init(argc, argv, 32, 64);
    stream_ms(jit(100, 0.2), S.big_bytes);  /* base layer */
    while (running()) {
        double progress = t_ms() / (S.duration_s * 1000.0);  /* 0 -> 1 over the visit */
        chase_ms(jit(40 + 90 * progress, 0.3)); /* tile lookups */
        stream_ms(jit(10, 0.3), 16 * MiB);  /* rasterize */
        idle_ms(urange(50, 130));
    }
    return site_exit();
}
