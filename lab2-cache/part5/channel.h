/* Part 5 - constants the sender and receiver agree on IN ADVANCE (like a protocol spec).
 * Including the same header from both sides is fine: it shares numbers, not memory.
 * Change anything you like. */
#ifndef CHANNEL_H
#define CHANNEL_H
#include "lab.h"

/* L2 sets used as the channel (with huge pages both sides can compute addresses for them). */
#define CH_NSETS 4
static const unsigned ch_sets[CH_NSETS] = {0x0A3, 0x1B7, 0x2C5, 0x3D9};

/* Duration of one transmitted symbol, in ticks. Both sides align symbols to plat_now(). */
#define CH_SLOT_TICKS ((uint64_t)(plat_is_sim() ? 40000 : 4000000))

#endif
