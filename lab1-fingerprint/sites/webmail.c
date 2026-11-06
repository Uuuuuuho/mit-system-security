/* webmail: a single-page app. Mixed load (graph walk + streaming), then a background poll every
 * ~400 ms (walk the message index, re-render) plus small typing bursts in between. */
#include "site.h"

int main(int argc, char **argv) {
    site_init(argc, argv, 24, 16);
    chase_ms(jit(90, 0.2));                 /* load the app state */
    stream_ms(jit(90, 0.2), S.big_bytes);   /* render the inbox */
    double next_poll = t_ms() + jit(200, 0.3);
    while (running()) {
        if (t_ms() >= next_poll) {
            chase_ms(jit(35, 0.25));        /* poll + diff */
            stream_ms(jit(15, 0.25), 8 * MiB);
            next_poll += jit(400, 0.1);
        } else {
            compute_ms(jit(3, 0.5));        /* keystroke */
            idle_ms(urange(30, 90));
        }
    }
    return site_exit();
}
