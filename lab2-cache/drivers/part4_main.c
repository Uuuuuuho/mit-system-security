/* part4_main.c - driver for Part 4 (secret key from key-dependent code paths). Do not edit.
 *
 *   ./bin/part4 [--seed N] [--bits K]      attack: recover a K-bit key (default 2)
 *   ./bin/part4 --defend [--bits K]        same, but the victim uses YOUR victim_lookup_ct()
 *   ./bin/part4 --ct-check                 grade victim_lookup_ct(): same result, key-independent trace
 *
 * Victim layout and schedule are described in include/lab.h (KEY_*). It runs on your SMT sibling. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "lab.h"

typedef struct {
    uint8_t key[256];
    int nbits;
    int defended;
    uint64_t seed;
} key_victim_t;

static uint64_t xs(uint64_t *s) {
    *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
    return *s;
}

static void fill_tables(uint8_t *page0, uint8_t *page1, uint64_t *r) {
    for (int i = 0; i < 4096; i++) { page0[i] = (uint8_t)xs(r); page1[i] = (uint8_t)xs(r); }
}

static void victim(void *arg) {
    key_victim_t *v = arg;
    uint64_t r = v->seed | 1;
    uint8_t *page0 = plat_alloc(4096, 0), *page1 = plat_alloc(4096, 0);
    fill_tables(page0, page1, &r);
    const uint8_t *t_zero = page0 + KEY_TABLE_ZERO_OFF, *t_one = page1 + KEY_TABLE_ONE_OFF;
    size_t wlen = 1u << 20;
    uint8_t *work = plat_alloc(wlen, 0);
    uint64_t slot = KEY_SLOT_TICKS, nslots = (uint64_t)(KEY_GAP_SLOTS + v->nbits);
    uint64_t base = plat_now() - xs(&r) % (slot * nslots); /* random phase */
    volatile uint8_t sink = 0;
    for (;;) {
        uint64_t s = ((plat_now() - base) / slot) % nslots;
        if (s >= KEY_GAP_SLOTS) {
            int bit = v->key[s - KEY_GAP_SLOTS];
            for (unsigned i = 0; i < KEY_TABLE_LINES; i++)
                sink ^= v->defended ? victim_lookup_ct(bit, i, t_zero, t_one) : victim_lookup(bit, i, t_zero, t_one);
        }
        for (int i = 0; i < 3; i++) plat_access(work + (xs(&r) % (wlen / 64)) * 64); /* unrelated work */
        plat_wait(300 + xs(&r) % 300);
    }
}

/* Exercise 4-3 grader: identical cache-line trace for bit=0 and bit=1, and the right values. */
static void ct_check(void) {
    if (!plat_is_sim()) hx_result(0, "--ct-check needs the sim backend (it records access traces)");
    uint64_t r = hx_secret_rand() | 1;
    uint8_t *page0 = plat_alloc(4096, 0), *page1 = plat_alloc(4096, 0);
    fill_tables(page0, page1, &r);
    const uint8_t *t0 = page0 + KEY_TABLE_ZERO_OFF, *t1 = page1 + KEY_TABLE_ONE_OFF;
    static uint64_t tr0[4096], tr1[4096];
    int bad_val = 0, bad_trace = 0, empty = 0;
    for (unsigned i = 0; i < KEY_TABLE_LINES; i++) {
        hx_trace_start();
        uint8_t v0 = victim_lookup_ct(0, i, t0, t1);
        int n0 = hx_trace_stop(tr0, 4096);
        hx_trace_start();
        uint8_t v1 = victim_lookup_ct(1, i, t0, t1);
        int n1 = hx_trace_stop(tr1, 4096);
        if (v0 != t0[i * 64] || v1 != t1[i * 64]) bad_val++;
        if (n0 != n1 || memcmp(tr0, tr1, sizeof(uint64_t) * (size_t)n0) != 0) bad_trace++;
        if (n0 == 0) empty++;
        printf("[part4] i=%u  bit0 -> %3u (want %3u), %d lines   bit1 -> %3u (want %3u), %d lines\n", i, v0,
               t0[i * 64], n0, v1, t1[i * 64], n1);
    }
    if (empty) hx_result(0, "victim_lookup_ct must read the tables through plat_read8()/plat_access()");
    hx_result(!bad_val && !bad_trace, "ct-check: wrong values for %d/%d indexes, key-dependent trace for %d/%d",
              bad_val, KEY_TABLE_LINES, bad_trace, KEY_TABLE_LINES);
}

int main(int argc, char **argv) {
    hx_init(argc, argv, "part4");
    if (hx_flag("--ct-check")) ct_check();
    hx_set_timeout(120, (uint64_t)8e9);
    static key_victim_t v;
    v.nbits = hx_arg("--bits") ? atoi(hx_arg("--bits")) : 2;
    if (v.nbits < 1 || v.nbits > 256) v.nbits = 2;
    v.defended = hx_flag("--defend");
    v.seed = hx_secret_rand();
    for (int i = 0; i < v.nbits; i++) v.key[i] = (uint8_t)(hx_secret_rand() & 1);

    cal_t cal = calibrate();
    printf("[part4] calibration: l2_miss=%llu\n", (unsigned long long)cal.l2_miss);
    printf("[part4] victim: %d-bit key, slot=%llu ticks, %s lookups\n", v.nbits, (unsigned long long)KEY_SLOT_TICKS,
           v.defended ? "constant-trace (your victim_lookup_ct)" : "leaky");
    hx_spawn(victim, &v, HX_SIBLING, "key-victim");

    uint8_t guess[256];
    memset(guess, 0xff, sizeof guess);
    uint64_t t0 = plat_now();
    int rc = leak_key(&cal, v.nbits, guess);
    uint64_t t1 = plat_now();
    char gs[300], ks[300];
    int correct = 0;
    for (int i = 0; i < v.nbits; i++) {
        gs[i] = guess[i] == 0 ? '0' : guess[i] == 1 ? '1' : '?';
        ks[i] = (char)('0' + v.key[i]);
        correct += guess[i] == v.key[i];
    }
    gs[v.nbits] = ks[v.nbits] = 0;
    printf("[part4] attack took %.2f Mticks, %d/%d bits right\n", (double)(t1 - t0) / 1e6, correct, v.nbits);
    hx_result(rc == 0 && correct == v.nbits, "%sguess=%s key=%s", v.defended ? "defended " : "", gs, ks);
}
