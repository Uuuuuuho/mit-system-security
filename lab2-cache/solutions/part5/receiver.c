/* Part 5 - REFERENCE SOLUTION (spoilers): covert channel receiver. Protocol in channel.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "channel.h"

static uint8_t *lines[CH_NSETS][CH_MAXW];
static int nlines;
static const cal_t *calp;

static void wait_until(uint64_t t) {
    for (uint64_t now = plat_now(); now < t; now = plat_now()) plat_wait(t - now < 1000 ? t - now : 1000);
}

static int probe_all(void) {
    int m = 0;
    for (int s = 0; s < CH_NSETS; s++) m += pp_probe(lines[s], nlines, calp);
    return m;
}

/* Average misses per probe round inside [a, b), staying clear of the edges. */
static double measure(uint64_t a, uint64_t b) {
    uint64_t margin = (b - a) / 10;
    wait_until(a + margin);
    probe_all(); /* re-prime: evictions from before this half must not count */
    long m = 0;
    int r = 0;
    uint64_t last = plat_now(), dur = 0;
    while (plat_now() + dur < b - margin) {
        plat_wait(200);
        m += probe_all();
        r++;
        uint64_t now = plat_now();
        dur = now - last;
        last = now;
    }
    return r ? (double)m / r : 0;
}

int receiver_main(char *out, size_t cap, const cal_t *cal) {
    const plat_geom_t *g = plat_geom();
    if (cap) out[0] = 0;
    calp = cal;
    nlines = g->l2_ways < CH_MAXW ? g->l2_ways : CH_MAXW;
    if (ch_lines(lines, nlines)) { fprintf(stderr, "receiver: no huge pages\n"); return -1; }
    for (int s = 0; s < CH_NSETS; s++) pp_prime(lines[s], nlines);

    size_t maxframes = cap / CH_CHUNK + 1;
    uint8_t *have = calloc(maxframes, 1);
    long total = -1, msglen = -1;
    double idle_level = 0.06 * CH_NSETS * nlines; /* "busy" = lost >6% of the lines per round */

    /* parser state */
    int in_frame = 0, since_idle = 0, nbits = 0;
    unsigned shift = 0;
    uint8_t fr[4 + 256 + 2];
    size_t fr_len = 0, fr_need = 0;
    long quiet = 0, seen_activity = 0, no_progress = 0;

    uint64_t slot = CH_SLOT_TICKS, half = CH_HALF_TICKS;
    uint64_t s = (plat_now() / slot + 1) * slot;
    for (;; s += slot) {
        double e1 = measure(s, s + half), e2 = measure(s + half, s + slot);
        int sym = (e1 < idle_level && e2 < idle_level) ? SYM_IDLE : (e1 > e2 ? SYM_1 : SYM_0);

        /* give up when the sender has clearly finished (or never started) */
        if (sym == SYM_IDLE) quiet++;
        else { quiet = 0; seen_activity = 1; }
        if ((seen_activity && quiet > 600) || (!seen_activity && quiet > 3000)) break;
        if (++no_progress > 4000) break; /* no valid frame for a long time: the channel is dead */

        if (sym == SYM_IDLE) { in_frame = 0; since_idle = 0; shift = 0; continue; }
        since_idle++;
        if (!in_frame) {
            shift = (shift << 1) | (unsigned)sym;
            if (since_idle == 8 && (shift & 0xff) == CH_SYNC) { in_frame = 1; fr_len = 0; fr_need = 3; nbits = 0; shift = 0; }
            continue;
        }
        shift = (shift << 1) | (unsigned)sym;
        if (++nbits < 8) continue;
        fr[fr_len++] = (uint8_t)shift;
        nbits = 0;
        shift = 0;
        if (fr_len == 3) fr_need = 3 + (size_t)fr[2] + 2;
        if (fr_len == 3 && fr[2] > CH_CHUNK) { in_frame = 0; continue; }
        if (fr_len < fr_need) continue;

        /* complete frame: fr = SEQhi SEQlo LEN payload CRChi CRClo */
        in_frame = 0;
        size_t plen = fr[2];
        uint16_t crc = (uint16_t)(fr[3 + plen] << 8 | fr[4 + plen]);
        if (crc != ch_crc16(fr, 3 + plen)) continue;
        unsigned seq = (unsigned)((fr[0] << 8 | fr[1]) & 0x7fff);
        int last = (fr[0] & 0x80) != 0;
        if (seq >= maxframes || (size_t)seq * CH_CHUNK + plen >= cap) continue;
        memcpy(out + (size_t)seq * CH_CHUNK, fr + 3, plen);
        have[seq] = 1;
        no_progress = 0;
        if (last) { total = seq + 1; msglen = (long)seq * CH_CHUNK + (long)plen; }
        if (total > 0) {
            long ok = 1;
            for (long i = 0; i < total; i++) ok &= have[i];
            if (ok) {
                out[msglen] = 0;
                free(have);
                return (int)msglen;
            }
        }
    }
    free(have);
    return -1;
}
