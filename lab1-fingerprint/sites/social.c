/* social: an infinite-scroll feed. Each "scroll" walks a large object graph (pointer chasing)
 * and decodes a thumbnail (compute); the user pauses irregularly between scrolls. */
#include "site.h"

int main(int argc, char **argv) {
    site_init(argc, argv, 0, 32);
    chase_ms(jit(120, 0.2));                /* initial feed render */
    while (running()) {
        chase_ms(jit(70, 0.3));             /* scroll: walk the feed */
        compute_ms(jit(25, 0.3));           /* thumbnail decode */
        idle_ms(urange(60, 180));           /* user reads */
    }
    return site_exit();
}
