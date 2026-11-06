/* Part 5 - REFERENCE SOLUTION (spoilers): the protocol both sides agree on.
 *
 * Physical layer: CH_NSETS L2 sets. Time is cut into symbols of CH_SLOT_TICKS, aligned to plat_now()
 * (the shared time base), each split into two halves. The sender "is busy" in a half by thrashing
 * the channel sets with more lines than they have ways; the receiver Prime+Probes them and averages
 * its misses per half. Manchester coding makes every bit self-referencing:
 *     bit 1 = busy|idle      bit 0 = idle|busy      IDLE = idle|idle
 * so no absolute threshold is needed to tell 0 from 1 (only to tell bits from IDLE).
 *
 * Link layer: the message is cut into frames of at most CH_CHUNK bytes:
 *     [SYNC 0xA5][SEQ hi][SEQ lo, bit 15 = last frame][LEN][payload ...][CRC16 hi][CRC16 lo]
 * Every frame is preceded by two IDLE symbols and sent CH_COPIES times. The receiver keeps every
 * frame whose CRC verifies and stops when it has frames 0..last. Any length works.
 */
#ifndef CHANNEL_H
#define CHANNEL_H
#include "lab.h"

#define CH_NSETS 4
static const unsigned ch_sets[CH_NSETS] = {0x0A3, 0x1B7, 0x2C5, 0x3D9};

#define CH_HALF_TICKS ((uint64_t)(plat_is_sim() ? 20000 : 2000000))
#define CH_SLOT_TICKS (2 * CH_HALF_TICKS)
#define CH_SYNC 0xA5
#define CH_CHUNK 32
#define CH_COPIES 2
#define CH_LEAD_SLOTS 8 /* idle symbols before the first frame (receiver warm-up) */
#define CH_MAXW 64

enum { SYM_0 = 0, SYM_1 = 1, SYM_IDLE = 2 };

static inline uint16_t ch_crc16(const uint8_t *d, size_t n) {
    uint16_t c = 0xFFFF; /* CRC-16/CCITT-FALSE */
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)(d[i] << 8);
        for (int b = 0; b < 8; b++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1);
    }
    return c;
}

/* Eviction sets for the channel sets, n lines each, from a fresh huge-page pool. */
static inline int ch_lines(uint8_t *lines[CH_NSETS][CH_MAXW], int n) {
    const plat_geom_t *g = plat_geom();
    pool_t pool;
    if (pool_init(&pool, (size_t)g->l2_sets * (size_t)g->line * (size_t)g->l2_ways * 2) || !pool.huge) return -1;
    for (int i = 0; i < CH_NSETS; i++)
        if (evset_build(&pool, ch_sets[i] % (unsigned)g->l2_sets, n, lines[i]) < n) return -1;
    return 0;
}

#endif
