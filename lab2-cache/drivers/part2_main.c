/* part2_main.c - driver for Part 2 (Flush+Reload CTF). Do not edit.
 *
 *   ./bin/part2 [--seed N] [--same-core]
 *
 * Starts a victim on the OTHER core (or, with --same-core, on your SMT sibling / your own CPU) that
 * keeps touching shared + secret*FR_STRIDE (plus decoys), then calls your fr_attack(). */
#include <stdio.h>
#include <stdlib.h>

#include "harness.h"
#include "lab.h"

typedef struct {
    const uint8_t *shared;
    int secret;
    uint64_t seed;
} fr_victim_t;

static uint64_t xs(uint64_t *s) {
    *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
    return *s;
}

/* The victim: a "library" routine indexed by a secret, plus some unrelated work. */
static void victim(void *arg) {
    fr_victim_t *v = arg;
    uint64_t r = v->seed | 1;
    uint8_t *work = plat_alloc(256 * 1024, 0);
    for (;;) {
        plat_access(v->shared + (size_t)v->secret * FR_STRIDE);       /* the secret-dependent access */
        if (xs(&r) % 3 == 0)                                          /* decoy: some other slot      */
            plat_access(v->shared + (size_t)(xs(&r) % FR_SLOTS) * FR_STRIDE);
        for (int i = 0; i < 4; i++) plat_access(work + (xs(&r) % (256 * 1024 / 64)) * 64);
        plat_wait(600 + xs(&r) % 1800);
    }
}

int main(int argc, char **argv) {
    hx_init(argc, argv, "part2");
    hx_set_timeout(90, (uint64_t)4e9);
    cal_t cal = calibrate();
    printf("[part2] calibration: llc_miss=%llu (DRAM median %llu, LLC median %llu)\n",
           (unsigned long long)cal.llc_miss, (unsigned long long)cal.med[LVL_DRAM], (unsigned long long)cal.med[LVL_LLC]);

    const uint8_t *shared = hx_shared_buf((size_t)FR_SLOTS * FR_STRIDE);
    static fr_victim_t v;
    v.shared = shared;
    v.secret = (int)(hx_secret_rand() % FR_SLOTS);
    v.seed = hx_secret_rand();
    hx_spawn(victim, &v, hx_flag("--same-core") ? HX_SIBLING : HX_OTHER_CORE, "fr-victim");

    uint64_t t0 = plat_now();
    int guess = fr_attack(shared, &cal);
    uint64_t t1 = plat_now();
    printf("[part2] attack took %.2f Mticks\n", (double)(t1 - t0) / 1e6);
    hx_result(guess == v.secret, "guess=%d secret=%d", guess, v.secret);
}
