/* shd.h - tiny portable helpers for the shd-labs microarchitecture exercises.
 *
 * Supported: Linux on x86_64 and aarch64 (native, VM or Docker; NOT under x86 emulation).
 *   timer : x86_64 -> rdtscp (cycles of the TSC)      aarch64 -> cntvct_el0 (generic timer ticks)
 *           SHD_TIMER=thread selects a counting-thread timer on either arch (finer than cntvct on Apple silicon).
 *   flush : x86_64 -> clflush                          aarch64 -> dc civac
 *   fence : x86_64 -> mfence/lfence                    aarch64 -> dsb ish + isb
 * Everything is header-only: #include "shd.h" and link with -pthread.
 */
#ifndef SHD_H
#define SHD_H
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(__x86_64__) && !defined(__aarch64__)
#error "shd.h supports x86_64 and aarch64 only"
#endif

/* ---------------- fences ---------------- */
static inline void shd_mfence(void) {
#if defined(__x86_64__)
    __asm__ volatile("mfence" ::: "memory");
#else
    __asm__ volatile("dsb ish" ::: "memory");
#endif
}
static inline void shd_lfence(void) {
#if defined(__x86_64__)
    __asm__ volatile("lfence" ::: "memory");
#else
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
#endif
}

/* ---------------- cache maintenance ---------------- */
static inline void shd_flush(const void *p) {
#if defined(__x86_64__)
    __asm__ volatile("clflush (%0)" ::"r"(p) : "memory");
#else
    __asm__ volatile("dc civac, %0" ::"r"(p) : "memory");
#endif
}
static inline void shd_access(const void *p) { (void)*(volatile const uint8_t *)p; }

/* ---------------- timers ---------------- */
static volatile uint64_t shd__counter;
static int shd__mode = -1; /* 0 = native, 1 = counting thread */

static void *shd__count_loop(void *arg) {
    (void)arg;
    for (;;) {
#if defined(__x86_64__)
        __asm__ volatile("" ::: "memory");
#endif
        shd__counter++;
    }
    return NULL;
}

static inline uint64_t shd__native(void) {
#if defined(__x86_64__)
    uint32_t lo, hi, aux;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux)::"memory");
    return ((uint64_t)hi << 32) | lo;
#else
    uint64_t v;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)::"memory");
    return v;
#endif
}

/* Call once at program start. Reads SHD_TIMER=native|thread (default: native). */
static inline void shd_timer_init(void) {
    if (shd__mode >= 0) return;
    const char *m = getenv("SHD_TIMER");
    shd__mode = (m && strcmp(m, "thread") == 0) ? 1 : 0;
    if (shd__mode == 1) {
        pthread_t t;
        if (pthread_create(&t, NULL, shd__count_loop, NULL) != 0) { perror("pthread_create"); exit(1); }
        pthread_detach(t);
        while (shd__counter < 1000000) { /* let the counter thread spin up */ }
    }
}
static inline const char *shd_timer_name(void) {
    if (shd__mode == 1) return "counting-thread";
#if defined(__x86_64__)
    return "rdtscp";
#else
    return "cntvct_el0";
#endif
}
static inline uint64_t shd_now(void) {
    if (shd__mode == 1) { shd_lfence(); uint64_t v = shd__counter; shd_lfence(); return v; }
    return shd__native();
}

/* Time one load of p (in timer units). */
static inline uint64_t shd_time_access(const void *p) {
    shd_mfence(); shd_lfence();
    uint64_t t0 = shd_now();
    shd_lfence();
    shd_access(p);
    shd_lfence();
    uint64_t t1 = shd_now();
    return t1 - t0;
}

/* ---------------- misc ---------------- */
static inline int shd_pin_cpu(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    return sched_setaffinity(0, sizeof(s), &s);
}
static inline long shd_line_size(void) {
    long v = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (v <= 0) {
        FILE *f = fopen("/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size", "r");
        if (f) { if (fscanf(f, "%ld", &v) != 1) v = 0; fclose(f); }
    }
    return v > 0 ? v : 64;
}
/* Page-aligned, pre-faulted buffer. Tries 2 MiB huge pages first when huge=1. */
static inline void *shd_alloc(size_t bytes, int huge) {
    void *p = MAP_FAILED;
#ifdef MAP_HUGETLB
    if (huge) p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
#endif
    if (p == MAP_FAILED) {
        p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (p == MAP_FAILED) { perror("mmap"); exit(1); }
        if (huge) madvise(p, bytes, MADV_HUGEPAGE);
    }
    memset(p, 1, bytes);
    return p;
}
static int shd__cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static inline uint64_t shd_median(uint64_t *v, size_t n) {
    qsort(v, n, sizeof *v, shd__cmp_u64);
    return v[n / 2];
}
#endif
