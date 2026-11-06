/* fp.h - small helpers shared by the lab1 C programs (collectors, sites, timer warm-up).
 *
 * Everything here is plain POSIX + Linux sysfs, so it works the same on x86_64 and aarch64
 * (bare metal, VM or Docker). CPU pinning and the line size come from ../../common/shd.h.
 */
#ifndef FP_H
#define FP_H
#include "shd.h"

#include <errno.h>
#include <stdarg.h>
#include <time.h>

/* Monotonic wall clock in nanoseconds (vDSO clock_gettime: no syscall on x86_64/aarch64). */
static inline uint64_t fp_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void fp_sleep_ns(uint64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { /* resume */ }
}

static inline void fp_die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

/* Pin to `cpu` if it exists; -1 means "do not pin". Never fatal: a 1-CPU container just runs unpinned. */
static inline void fp_pin(int cpu) {
    if (cpu < 0) return;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0 && cpu >= n) cpu = (int)(cpu % n);
    if (shd_pin_cpu(cpu) != 0) fprintf(stderr, "note: could not pin to cpu %d, running unpinned\n", cpu);
}

/* Parse sysfs cache sizes such as "48K", "2048K", "8M". Returns bytes or 0. */
static inline size_t fp__parse_size(const char *s) {
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    if (*end == 'K' || *end == 'k') v <<= 10;
    else if (*end == 'M' || *end == 'm') v <<= 20;
    else if (*end == 'G' || *end == 'g') v <<= 30;
    return (size_t)v;
}

/* Size of the largest (last-level) unified/data cache seen by cpu0, from sysfs; 0 if unknown.
 * Many arm64 VMs (including Docker Desktop on Apple silicon) expose no cache info at all. */
static inline size_t fp_llc_bytes(void) {
    size_t best = 0;
    int best_level = 0;
    for (int i = 0; i < 16; i++) {
        char path[128], buf[64];
        int level = 0;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu0/cache/index%d/level", i);
        FILE *f = fopen(path, "r");
        if (!f) break;
        if (fscanf(f, "%d", &level) != 1) level = 0;
        fclose(f);
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu0/cache/index%d/type", i);
        f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
        fclose(f);
        if (strncmp(buf, "Instruction", 11) == 0) continue;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu0/cache/index%d/size", i);
        f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
        fclose(f);
        size_t sz = fp__parse_size(buf);
        if (level > best_level || (level == best_level && sz > best)) { best_level = level; best = sz; }
    }
    return best;
}

/* Small deterministic PRNG (splitmix64) so every program can be re-run with the same --seed. */
static inline uint64_t fp_rand(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
/* Uniform double in [0,1). */
static inline double fp_uniform(uint64_t *s) { return (double)(fp_rand(s) >> 11) * (1.0 / 9007199254740992.0); }

#endif
