/* part5_main.c - driver for Part 5 (covert "dead drop"). Do not edit.
 *
 *   ./bin/part5 [--seed N] [--msg "text"] [--len N]
 *
 * Starts your receiver (SMT sibling) and your sender (hart 0 of the same core) as separate parties
 * that share no writable memory; plat_flush() is disabled. The message is chosen after the receiver
 * has started. PASS if the receiver returns exactly the sent message. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "lab.h"

#define MAXMSG 4096
static cal_t cal;

static void receiver_party(void *arg) {
    (void)arg;
    static char out[MAXMSG + 1];
    int n = receiver_main(out, sizeof out, &cal);
    if (n < 0) n = 0;
    out[MAXMSG] = 0;
    hx_return(out, strnlen(out, MAXMSG));
}

static void sender_party(void *arg) {
    sender_main((const char *)arg, &cal);
}

int main(int argc, char **argv) {
    hx_init(argc, argv, "part5");
    hx_set_timeout(150, (uint64_t)20e9);
    cal = calibrate(); /* before flush is disabled; both parties get a copy */
    printf("[part5] calibration: l2_miss=%llu\n", (unsigned long long)cal.l2_miss);
    hx_disable_flush();
    int rid = hx_spawn(receiver_party, NULL, HX_SIBLING, "receiver");

    static char msg[MAXMSG + 1];
    if (hx_arg("--msg")) {
        snprintf(msg, sizeof msg, "%s", hx_arg("--msg"));
    } else {
        static const char *words[] = {"meet", "at", "the", "old", "oak", "tree", "bring", "two", "maps", "cache",
                                      "line", "noon", "dusk", "key", "is", "under", "blue", "stone", "north", "gate"};
        int want = hx_arg("--len") ? atoi(hx_arg("--len")) : 24 + (int)(hx_secret_rand() % 25);
        if (want > MAXMSG) want = MAXMSG;
        while ((int)strlen(msg) < want) {
            const char *w = words[hx_secret_rand() % 20];
            if (strlen(msg) + strlen(w) + 1 > (size_t)want) break;
            if (msg[0]) strcat(msg, " ");
            strcat(msg, w);
        }
        char tag[16];
        snprintf(tag, sizeof tag, " #%03u", (unsigned)(hx_secret_rand() % 1000));
        strcat(msg, tag);
    }
    hx_spawn(sender_party, msg, HX_HART0, "sender");

    static char got[MAXMSG + 1];
    uint64_t t0 = plat_now();
    int n = hx_join(rid, got, sizeof got);
    uint64_t t1 = plat_now();
    double mt = (double)(t1 - t0) / 1e6;
    printf("[part5] SENT:     \"%s\" (%zu bytes)\n", msg, strlen(msg));
    printf("[part5] RECEIVED: \"%s\"\n", n >= 0 ? got : "");
    if (mt > 0.01) printf("[part5] took %.1f Mticks  (%.1f payload bits per Mtick)\n", mt, 8.0 * (double)strlen(msg) / mt);
    hx_result(n >= 0 && strcmp(got, msg) == 0, "len=%zu", strlen(msg));
}
