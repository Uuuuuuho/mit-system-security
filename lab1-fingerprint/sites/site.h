/* site.h - building blocks for the synthetic "websites" (victim workloads).
 *
 * A site is a tiny C program that runs for --duration seconds and produces a characteristic
 * *temporal* pattern of memory traffic, CPU use and idleness, just like a real page load has a
 * characteristic pattern of parsing, layout, image decoding, video frames, timers ...
 *
 * Every run is randomized (--seed): start offset, phase lengths and intensities jitter by
 * roughly +-15-25%, so no two traces of the same site are identical and a classifier has to
 * learn the *shape*, not memorize a trace.
 *
 * Usage of every site:  ./sites/<name> [--duration S] [--seed N] [--cpu C]
 *
 * These programs only touch their own memory. They are the cooperating "victims" of this lab.
 */
#ifndef SITE_H
#define SITE_H
#include "fp.h"

#define MiB (1024ull * 1024ull)

typedef struct {
    double duration_s;  /* how long to run */
    uint64_t seed;      /* jitter seed */
    uint64_t rng;       /* PRNG state */
    uint64_t t0;        /* start time (ns) */
    size_t line;        /* cache-line size */
    uint8_t *big;       /* streaming buffer */
    size_t big_bytes;
    size_t *chain;      /* pointer-chasing ring (indices into itself) */
    size_t chain_len;
    volatile uint64_t sink;
} site_t;

static site_t S;

/* ---- jitter helpers ---- */
/* x scaled by a random factor in [1-f, 1+f]. */
static inline double jit(double x, double f) { return x * (1.0 - f + 2.0 * f * fp_uniform(&S.rng)); }
static inline double urange(double lo, double hi) { return lo + (hi - lo) * fp_uniform(&S.rng); }
/* Milliseconds since the site started. */
static inline double t_ms(void) { return (double)(fp_now_ns() - S.t0) / 1e6; }
static inline int running(void) { return t_ms() < S.duration_s * 1000.0; }

/* ---- workload primitives (each runs for roughly `ms` milliseconds) ---- */

/* Idle: the CPU is released to the scheduler. */
static inline void idle_ms(double ms) {
    if (ms > 0) fp_sleep_ns((uint64_t)(ms * 1e6));
}

/* Streaming: read-modify-write sweeps over `bytes` of the big buffer (memory-bandwidth heavy,
 * evicts lots of cache lines: the strongest signal for a cache-occupancy collector). */
static inline void stream_ms(double ms, size_t bytes) {
    if (bytes > S.big_bytes) bytes = S.big_bytes;
    uint64_t end = fp_now_ns() + (uint64_t)(ms * 1e6);
    static size_t pos;
    while (fp_now_ns() < end) {
        /* one 256 KiB chunk between clock checks */
        for (size_t i = 0; i < 4096; i++) {
            S.big[pos] += 1;
            pos += S.line;
            if (pos >= bytes) pos = 0;
        }
    }
}

/* Pointer chasing: dependent loads over a random ring spread across the big buffer
 * (latency bound, touches many distinct lines and pages, little bandwidth). */
static inline void chase_ms(double ms) {
    uint64_t end = fp_now_ns() + (uint64_t)(ms * 1e6);
    static size_t p;
    while (fp_now_ns() < end) {
        for (int i = 0; i < 2000; i++) p = S.chain[p];
    }
    S.sink += p;
}

/* Compute: integer mixing in registers only (no memory traffic, full CPU use). */
static inline void compute_ms(double ms) {
    uint64_t end = fp_now_ns() + (uint64_t)(ms * 1e6);
    uint64_t x = S.sink | 1;
    while (fp_now_ns() < end) {
        for (int i = 0; i < 20000; i++) x = x * 6364136223846793005ull + 1442695040888963407ull, x ^= x >> 29;
    }
    S.sink = x;
}

/* Sleep until absolute site time `ms` (used for periodic schedules). */
static inline void idle_until(double ms) {
    double now = t_ms();
    if (ms > now) idle_ms(ms - now);
}

/* ---- setup ---- */
static inline void site_init(int argc, char **argv, size_t big_mib, size_t chain_mib) {
    S.duration_s = 2.0;
    S.seed = (uint64_t)fp_now_ns() ^ ((uint64_t)getpid() << 32);
    int cpu = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--duration") && i + 1 < argc) S.duration_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) S.seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) cpu = atoi(argv[++i]);
        else fp_die("usage: %s [--duration S] [--seed N] [--cpu C]", argv[0]);
    }
    S.rng = S.seed;
    fp_pin(cpu);
    S.line = (size_t)shd_line_size();

    /* Buffers are allocated (and page-faulted) lazily by the first phase that uses them, so the
     * "page load" itself shows up in the trace, as it would for a real site. */
    S.big_bytes = big_mib * MiB;
    S.big = big_mib ? malloc(S.big_bytes) : NULL;
    if (big_mib && !S.big) fp_die("out of memory");
    if (chain_mib) {
        /* random cyclic permutation over one slot per cache line (Sattolo's algorithm) */
        size_t slots = chain_mib * MiB / sizeof(size_t);
        size_t stride = S.line / sizeof(size_t);
        size_t n = slots / stride;
        S.chain = malloc(slots * sizeof(size_t));
        size_t *perm = malloc(n * sizeof(size_t));
        if (!S.chain || !perm) fp_die("out of memory");
        for (size_t i = 0; i < n; i++) perm[i] = i;
        for (size_t i = n - 1; i > 0; i--) {
            size_t j = fp_rand(&S.rng) % i;
            size_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
        }
        for (size_t i = 0; i < n; i++) S.chain[perm[i] * stride] = perm[(i + 1) % n] * stride;
        S.chain_len = n;
        free(perm);
    }
    /* Tell tools/collect.py that setup is done; it then releases the collector, so trace window 0
     * starts (roughly) when the site starts. */
    printf("ready\n");
    fflush(stdout);
    S.t0 = fp_now_ns();
    /* random start offset: the collector and the site are never perfectly aligned */
    idle_ms(urange(0.0, 25.0));
}

static inline int site_exit(void) { return (int)(S.sink & 0); }

#endif
