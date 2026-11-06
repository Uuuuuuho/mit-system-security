/* alloc_shim.c - see alloc_shim.h. Test infrastructure only; you do not need to modify it. */
#define _GNU_SOURCE
#include "alloc_shim.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t m);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);

/* Open-addressing hash table in static storage (the shim must not allocate itself). */
#define SLOTS (1u << 16)
#define TOMB ((void *)1)
static struct slot { void *p; size_t n; void *site; } tab[SLOTS];
static size_t live_blocks, live_bytes, injected;
static long fail_countdown = -1;
static int fill = -1; /* -1 = not decided yet */

static size_t h(const void *p) { return (size_t)(((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull) & (SLOTS - 1); }

static void die(const char *what, const void *p) {
    fprintf(stderr, "alloc_shim: %s: %p\n", what, p);
    fflush(stderr);
    abort();
}

static void add(void *p, size_t n, void *site) {
    size_t i = h(p);
    while (tab[i].p && tab[i].p != TOMB) i = (i + 1) & (SLOTS - 1);
    tab[i].p = p, tab[i].n = n, tab[i].site = site;
    live_blocks++, live_bytes += n;
    if (live_blocks > SLOTS / 2) die("too many live blocks for the shim table", p);
}

static struct slot *find(const void *p) {
    for (size_t i = h(p), k = 0; k < SLOTS; i = (i + 1) & (SLOTS - 1), k++) {
        if (tab[i].p == NULL) return NULL;
        if (tab[i].p == p) return &tab[i];
    }
    return NULL;
}

static void del(struct slot *s) {
    live_blocks--, live_bytes -= s->n;
    s->p = TOMB;
}

static int should_fail(void) {
    if (fail_countdown < 0) return 0;
    if (fail_countdown-- == 0) { injected++; return 1; }
    return 0;
}

static int fill_enabled(void) {
    if (fill < 0) fill = getenv("SHIM_NOFILL") ? 0 : 1;
    return fill;
}

void *__wrap_malloc(size_t n) {
    if (should_fail()) return NULL;
    void *p = __real_malloc(n);
    if (p) {
        if (fill_enabled()) memset(p, 0xA5, n);
        add(p, n, __builtin_return_address(0));
    }
    return p;
}

void *__wrap_calloc(size_t n, size_t m) {
    if (should_fail()) return NULL;
    void *p = __real_calloc(n, m);
    if (p) add(p, n * m, __builtin_return_address(0)); /* calloc already checked n*m */
    return p;
}

void *__wrap_realloc(void *old, size_t n) {
    if (old == NULL) return __wrap_malloc(n);
    struct slot *s = find(old);
    if (!s) die("realloc() of a pointer that is not currently allocated (use after free?)", old);
    if (should_fail()) return NULL; /* like real realloc: the old block stays valid */
    size_t oldn = s->n;
    void *site = s->site;
    void *p = __real_realloc(old, n);
    if (p == NULL && n != 0) return NULL;
    del(find(old));
    if (p) {
        if (n > oldn && fill_enabled()) memset((char *)p + oldn, 0xA5, n - oldn);
        add(p, n, site);
    }
    return p;
}

void __wrap_free(void *p) {
    if (p == NULL) return;
    struct slot *s = find(p);
    if (!s) die("free() of a pointer that is not currently allocated (double free or invalid free)", p);
    del(s);
    __real_free(p);
}

size_t shim_live_blocks(void) { return live_blocks; }
size_t shim_live_bytes(void) { return live_bytes; }
void shim_fail_after(long n) { fail_countdown = n; }
size_t shim_failures(void) { return injected; }

void shim_dump_live(int max) {
    for (size_t i = 0; i < SLOTS && max > 0; i++) {
        if (tab[i].p && tab[i].p != TOMB) {
            printf("    live block %p: %zu bytes, allocated from code address %p\n", tab[i].p, tab[i].n, tab[i].site);
            max--;
        }
    }
}
