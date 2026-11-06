/* backend.h - internal: the table of functions each backend (sim.c, native.c) provides. */
#ifndef BACKEND_H
#define BACKEND_H
#include <stddef.h>
#include <stdint.h>
#include "harness.h"
#include "plat.h"

typedef struct {
    const char *name;
    void (*init)(uint64_t seed);
    const plat_geom_t *(*geom)(void);
    void *(*alloc)(size_t bytes, int huge);
    int (*is_huge)(const void *p);
    void (*access)(const void *p);
    uint8_t (*read8)(const void *p);
    uint64_t (*probe)(const void *p);
    void (*flush)(const void *p);
    void (*fence)(void);
    uint64_t (*now)(void);
    void (*wait)(uint64_t ticks);
    int (*spawn)(void (*fn)(void *), void *arg, hx_place_t where, const char *name);
    void (*ret)(const void *data, size_t len);
    int (*join)(int id, char *out, size_t cap);
    const uint8_t *(*shared_buf)(size_t bytes);
    uint64_t (*pa_of)(const void *p);
    void (*trace_start)(void);
    int (*trace_stop)(uint64_t *lines, int cap);
    void (*set_timeout)(unsigned wall_seconds, uint64_t virtual_ticks);
} backend_t;

extern const backend_t sim_backend;
extern const backend_t native_backend;

/* shared helpers implemented in plat.c */
extern int hx__flush_disabled;
void hx__die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* splitmix64 / xoshiro256** PRNG used everywhere */
typedef struct { uint64_t s[4]; } rng_t;
void rng_seed(rng_t *r, uint64_t seed);
uint64_t rng_next(rng_t *r);
static inline double rng_unit(rng_t *r) { return (double)(rng_next(r) >> 11) * (1.0 / 9007199254740992.0); }

#endif
