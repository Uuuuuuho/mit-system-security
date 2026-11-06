/* Part 5 - REFERENCE SOLUTION (spoilers): covert channel sender. Protocol in channel.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "channel.h"

static uint8_t *lines[CH_NSETS][CH_MAXW];
static int nlines;

static void idle_until(uint64_t t) {
    for (uint64_t now = plat_now(); now < t; now = plat_now()) plat_wait(t - now < 1000 ? t - now : 1000);
}

/* Keep the channel sets full of our lines until time t. */
static void busy_until(uint64_t t) {
    while (plat_now() < t) {
        for (int s = 0; s < CH_NSETS; s++)
            for (int i = 0; i < nlines; i++) plat_access(lines[s][i]);
        plat_wait(100); /* native, same CPU: let the receiver get scheduled */
    }
}

static int put_byte(uint8_t *sym, int k, uint8_t b) {
    for (int i = 7; i >= 0; i--) sym[k++] = (uint8_t)((b >> i) & 1);
    return k;
}

void sender_main(const char *msg, const cal_t *cal) {
    (void)cal;
    const plat_geom_t *g = plat_geom();
    nlines = 2 * g->l2_ways; /* twice the ways: real L2s (not LRU) need that to evict reliably */
    if (nlines > CH_MAXW) nlines = CH_MAXW;
    if (ch_lines(lines, nlines)) { fprintf(stderr, "sender: no huge pages\n"); return; }

    /* build the whole symbol stream first */
    size_t len = strlen(msg);
    size_t nframes = len ? (len + CH_CHUNK - 1) / CH_CHUNK : 1;
    size_t per_frame = 2 + 8 * (4 + CH_CHUNK + 2);
    uint8_t *sym = malloc(CH_LEAD_SLOTS + nframes * CH_COPIES * per_frame + 4);
    int k = 0;
    for (int i = 0; i < CH_LEAD_SLOTS; i++) sym[k++] = SYM_IDLE;
    for (size_t f = 0; f < nframes; f++) {
        uint8_t fr[4 + CH_CHUNK + 2];
        size_t plen = len - f * CH_CHUNK < CH_CHUNK ? len - f * CH_CHUNK : CH_CHUNK;
        uint16_t seq = (uint16_t)(f | (f == nframes - 1 ? 0x8000u : 0));
        fr[0] = CH_SYNC;
        fr[1] = (uint8_t)(seq >> 8);
        fr[2] = (uint8_t)seq;
        fr[3] = (uint8_t)plen;
        memcpy(fr + 4, msg + f * CH_CHUNK, plen);
        uint16_t crc = ch_crc16(fr + 1, 3 + plen);
        fr[4 + plen] = (uint8_t)(crc >> 8);
        fr[5 + plen] = (uint8_t)crc;
        for (int c = 0; c < CH_COPIES; c++) {
            sym[k++] = SYM_IDLE;
            sym[k++] = SYM_IDLE;
            for (size_t i = 0; i < 6 + plen; i++) k = put_byte(sym, k, fr[i]);
        }
    }
    sym[k++] = SYM_IDLE;

    /* transmit, one symbol per slot, aligned to the shared clock */
    uint64_t slot = CH_SLOT_TICKS, half = CH_HALF_TICKS;
    uint64_t t0 = (plat_now() / slot + 1) * slot;
    for (int i = 0; i < k; i++) {
        uint64_t s = t0 + (uint64_t)i * slot;
        if (sym[i] == SYM_1) { idle_until(s); busy_until(s + half); }
        if (sym[i] == SYM_0) { idle_until(s + half); busy_until(s + slot); }
        idle_until(s + slot);
    }
    free(sym);
}
