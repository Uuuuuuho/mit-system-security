/* miner: a page running an in-browser "crypto miner" / heavy WebAssembly loop.
 * Almost continuous compute in registers with tiny yields; no significant memory traffic. */
#include "site.h"

int main(int argc, char **argv) {
    site_init(argc, argv, 4, 0);
    stream_ms(jit(30, 0.3), S.big_bytes);   /* small page */
    while (running()) {
        compute_ms(jit(45, 0.2));           /* hash rounds */
        idle_ms(jit(4, 0.5));               /* yield to the event loop */
    }
    return site_exit();
}
