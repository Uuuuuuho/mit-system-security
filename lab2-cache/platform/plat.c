/* plat.c - backend selection, command-line handling, PRNG and the plat_* / hx_* entry points.
 * Each entry point simply forwards to the active backend (sim.c or native.c). */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "backend.h"

static const backend_t *B;
static uint64_t g_seed;
static rng_t g_user_rng, g_secret_rng;
static int g_argc;
static char **g_argv;
static const char *g_part = "lab2";
int hx__flush_disabled;

/* ------------------------------------------------------------------ PRNG */
static uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
void rng_seed(rng_t *r, uint64_t seed) {
    for (int i = 0; i < 4; i++) r->s[i] = splitmix64(&seed);
}
static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
uint64_t rng_next(rng_t *r) {
    uint64_t *s = r->s, res = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t; s[3] = rotl(s[3], 45);
    return res;
}

/* ------------------------------------------------------------------ misc */
void hx__die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] fatal: ", g_part);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(NULL);
    printf("RESULT FAIL fatal error (see stderr)\n");
    fflush(stdout);
    _exit(2);
}

static void need_init(void) {
    if (!B) {
        fprintf(stderr, "plat: hx_init() was not called\n");
        abort();
    }
}

const char *hx_arg(const char *name) {
    for (int i = 1; i + 1 < g_argc; i++)
        if (strcmp(g_argv[i], name) == 0) return g_argv[i + 1];
    return NULL;
}
int hx_flag(const char *name) {
    for (int i = 1; i < g_argc; i++)
        if (strcmp(g_argv[i], name) == 0) return 1;
    return 0;
}

void hx_init(int argc, char **argv, const char *part_name) {
    g_argc = argc;
    g_argv = argv;
    if (part_name) g_part = part_name;
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *s = hx_arg("--seed");
    if (!s) s = getenv("SEED");
    if (s && *s) {
        g_seed = strtoull(s, NULL, 0);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        g_seed = ((uint64_t)ts.tv_sec * 1000003u) ^ (uint64_t)ts.tv_nsec ^ ((uint64_t)getpid() << 20);
        g_seed &= 0xffffffffu;
    }
    rng_seed(&g_user_rng, g_seed ^ 0x5eed0001u);
    rng_seed(&g_secret_rng, g_seed ^ 0x5ec2e700u);

    const char *be = hx_arg("--backend");
    if (!be) be = getenv("SHD_BACKEND");
    if (!be || !*be || strcmp(be, "sim") == 0) B = &sim_backend;
    else if (strcmp(be, "native") == 0) B = &native_backend;
    else hx__die("unknown backend '%s' (use sim or native)", be);

    printf("[%s] backend=%s seed=%llu\n", g_part, B->name, (unsigned long long)g_seed);
    B->init(g_seed);
}

uint64_t hx_seed(void) { return g_seed; }
uint64_t hx_secret_rand(void) { return rng_next(&g_secret_rng); }

void hx_result(int pass, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fflush(stderr);
    printf("RESULT %s %s\n", pass ? "PASS" : "FAIL", msg);
    fflush(stdout);
    exit(0);
}

/* ------------------------------------------------------------------ plat_* */
const char *plat_backend(void) { need_init(); return B->name; }
int plat_is_sim(void) { need_init(); return B == &sim_backend; }
const plat_geom_t *plat_geom(void) { need_init(); return B->geom(); }
void *plat_alloc(size_t bytes, int huge) { need_init(); return B->alloc(bytes, huge); }
int plat_is_huge(const void *p) { return B->is_huge(p); }
void plat_access(const void *p) { B->access(p); }
uint8_t plat_read8(const void *p) { return B->read8(p); }
uint64_t plat_probe(const void *p) { return B->probe(p); }
void plat_flush(const void *p) {
    if (hx__flush_disabled) hx__die("plat_flush() is not allowed in this part (no flush instruction!)");
    B->flush(p);
}
void plat_fence(void) { B->fence(); }
uint64_t plat_now(void) { return B->now(); }
void plat_wait(uint64_t ticks) { B->wait(ticks); }
uint64_t plat_rand(void) { return rng_next(&g_user_rng); }

/* ------------------------------------------------------------------ hx_* */
int hx_spawn(void (*fn)(void *), void *arg, hx_place_t where, const char *name) {
    need_init();
    return B->spawn(fn, arg, where, name);
}
void hx_return(const void *data, size_t len) { B->ret(data, len); }
int hx_join(int id, char *out, size_t cap) { return B->join(id, out, cap); }
const uint8_t *hx_shared_buf(size_t bytes) { need_init(); return B->shared_buf(bytes); }
uint64_t hx_pa_of(const void *p) { return B->pa_of(p); }
void hx_disable_flush(void) { hx__flush_disabled = 1; }
void hx_trace_start(void) { B->trace_start(); }
int hx_trace_stop(uint64_t *lines, int cap) { return B->trace_stop(lines, cap); }
void hx_set_timeout(unsigned wall_seconds, uint64_t virtual_ticks) { B->set_timeout(wall_seconds, virtual_ticks); }
