/* native.c - the "native" backend: real caches, real timer, real processes.
 *
 * - Timing, flush and fences come from common/shd.h (rdtscp/clflush on x86_64, cntvct/dc civac on
 *   aarch64). SHD_TIMER=thread selects shd.h's counting-thread timer.
 * - Geometry is read from /sys/devices/system/cpu/cpu0/cache (with fallbacks).
 * - Parties are fork()ed processes pinned with sched_setaffinity:
 *     HX_SIBLING    -> the SMT sibling of the main CPU if one exists, otherwise the SAME CPU
 *                      (time-sliced; plat_wait() then calls sched_yield() so the parties alternate)
 *     HX_OTHER_CORE -> a CPU that is not a sibling of the main CPU (shares only the LLC)
 * - hx_shared_buf() is a memfd mapped read-only MAP_SHARED in every party (like a shared library).
 * - Huge pages: MAP_HUGETLB if the hugetlbfs pool has pages, else 2 MiB-aligned THP via madvise.
 */
#include "shd.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>

#include "backend.h"

#define HUGE_SZ (2ul << 20)

static plat_geom_t G;
static int cpu_main = 0, cpu_sib = -1, cpu_other = -1;
static int yield_mode;
static int is_child;
static int ret_fd = -1;

typedef struct { uintptr_t va; size_t len; int huge; } nregion_t;
static nregion_t regs[256];
static int nregs;

typedef struct { pid_t pid; int rfd; } child_t;
static child_t kids[16];
static int nkids;

/* ------------------------------------------------------------------ sysfs helpers */
static int read_sysfs_str(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)cap, f)) { fclose(f); return -1; }
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}
static long read_sysfs_long(const char *path) {
    char b[64];
    if (read_sysfs_str(path, b, sizeof b)) return -1;
    char *end;
    long v = strtol(b, &end, 10);
    if (*end == 'K') v *= 1024;
    else if (*end == 'M') v *= 1024 * 1024;
    return v;
}

static void read_geometry(void) {
    long line = shd_line_size();
    G.line = (int)line;
    G.page = (size_t)sysconf(_SC_PAGESIZE);
    G.huge = HUGE_SZ;
    /* fallbacks: a generic modern core */
    G.l1_sets = 64; G.l1_ways = 8;
    G.l2_sets = 1024; G.l2_ways = 16;
    G.llc_sets = 8192; G.llc_ways = 16;
    int have_l2 = 0, best_level = 0;
    for (int i = 0; i < 8; i++) {
        char p[128], type[32];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/type", cpu_main, i);
        if (read_sysfs_str(p, type, sizeof type)) break;
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/level", cpu_main, i);
        long level = read_sysfs_long(p);
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/ways_of_associativity", cpu_main, i);
        long ways = read_sysfs_long(p);
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/size", cpu_main, i);
        long size = read_sysfs_long(p);
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/number_of_sets", cpu_main, i);
        long sets = read_sysfs_long(p);
        if (ways <= 0 || size <= 0) continue;
        if (sets <= 0) sets = size / (ways * line);
        if (level == 1 && strcmp(type, "Instruction") != 0) { G.l1_sets = (int)sets; G.l1_ways = (int)ways; }
        if (level == 2 && strcmp(type, "Instruction") != 0) { G.l2_sets = (int)sets; G.l2_ways = (int)ways; have_l2 = 1; }
        if (level >= 2 && level > best_level && strcmp(type, "Unified") == 0) {
            best_level = (int)level;
            G.llc_sets = (int)sets;
            G.llc_ways = (int)ways;
        }
    }
    if (!have_l2) fprintf(stderr, "[native] warning: no L2 info in sysfs, assuming %d sets x %d ways\n", G.l2_sets, G.l2_ways);
}

static int parse_first_other(const char *list, int not_this, const cpu_set_t *allowed) {
    /* list like "0,4" or "0-1" */
    const char *s = list;
    while (*s) {
        char *end;
        long a = strtol(s, &end, 10), b = a;
        if (end == s) break;
        if (*end == '-') { s = end + 1; b = strtol(s, &end, 10); }
        for (long c = a; c <= b; c++)
            if (c != not_this && c < CPU_SETSIZE && CPU_ISSET((int)c, allowed)) return (int)c;
        s = end;
        if (*s == ',') s++;
        else break;
    }
    return -1;
}

static void choose_cpus(void) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof allowed, &allowed) != 0) { cpu_main = 0; return; }
    cpu_main = -1;
    for (int c = 0; c < CPU_SETSIZE; c++)
        if (CPU_ISSET(c, &allowed)) { cpu_main = c; break; }
    if (cpu_main < 0) cpu_main = 0;
    char p[128], sib[256] = "";
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu_main);
    if (read_sysfs_str(p, sib, sizeof sib) == 0) cpu_sib = parse_first_other(sib, cpu_main, &allowed);
    for (int c = 0; c < CPU_SETSIZE; c++) {
        if (!CPU_ISSET(c, &allowed) || c == cpu_main || c == cpu_sib) continue;
        cpu_other = c;
        break;
    }
}

/* ------------------------------------------------------------------ memory */
static uint64_t nat_pa_of(const void *p);

/* Is [va, va+len) really backed by 2 MiB pages?
 * With pagemap PFNs (root/CAP_SYS_ADMIN): every 2 MiB chunk must be physically contiguous and aligned.
 * Otherwise: /proc/self/smaps must report at least len bytes of AnonHugePages for the mapping. */
static int region_is_huge(uintptr_t va, size_t len) {
    if (nat_pa_of((void *)va) != ~0ull) {
        for (size_t off = 0; off < len; off += HUGE_SZ) {
            uint64_t a = nat_pa_of((void *)(va + off));
            uint64_t b = nat_pa_of((void *)(va + off + HUGE_SZ - 4096));
            uint64_t c = nat_pa_of((void *)(va + off + HUGE_SZ / 2));
            if (a == ~0ull || (a & (HUGE_SZ - 1)) || b != a + HUGE_SZ - 4096 || c != a + HUGE_SZ / 2) return 0;
        }
        return 1;
    }
    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f) return 0;
    char line[512];
    int inside = 0, huge = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b;
        if (sscanf(line, "%lx-%lx ", &a, &b) == 2) {
            if (inside) break;
            inside = (va >= a && va < b);
            continue;
        }
        if (!inside) continue;
        unsigned long kb;
        if (sscanf(line, "AnonHugePages: %lu kB", &kb) == 1 && kb * 1024ul >= len) huge = 1;
        if (sscanf(line, "KernelPageSize: %lu kB", &kb) == 1 && kb >= 2048) huge = 1;
    }
    fclose(f);
    return huge;
}

static void *nat_alloc(size_t bytes, int huge) {
    void *p;
    int got_huge = 0;
    if (!huge) {
        size_t len = (bytes + G.page - 1) / G.page * G.page;
        p = shd_alloc(len ? len : G.page, 0); /* pre-faulted, filled with 1s */
        if (nregs < 256) regs[nregs++] = (nregion_t){(uintptr_t)p, len, 0};
        return p;
    }
    size_t len = (bytes + HUGE_SZ - 1) / HUGE_SZ * HUGE_SZ;
    if (!len) len = HUGE_SZ;
    p = MAP_FAILED;
#ifdef MAP_HUGETLB
    p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
    if (p != MAP_FAILED) got_huge = 1;
#endif
    if (p == MAP_FAILED) {
        uint8_t *m = mmap(NULL, len + HUGE_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) hx__die("mmap(%zu) failed: %s", len, strerror(errno));
        uintptr_t a = ((uintptr_t)m + HUGE_SZ - 1) & ~(uintptr_t)(HUGE_SZ - 1);
        p = (void *)a;
#ifdef MADV_HUGEPAGE
        madvise(p, len, MADV_HUGEPAGE);
#endif
    }
    memset(p, 1, len);
    if (!got_huge) got_huge = region_is_huge((uintptr_t)p, len);
    if (!got_huge)
        fprintf(stderr, "[native] warning: could not get 2 MiB pages for a %zu-byte buffer "
                        "(no hugetlbfs pages, THP disabled?) - address bits above 11 are NOT physical\n", len);
    if (nregs < 256) regs[nregs++] = (nregion_t){(uintptr_t)p, len, got_huge};
    return p;
}

static int nat_is_huge(const void *p) {
    uintptr_t a = (uintptr_t)p;
    for (int i = 0; i < nregs; i++)
        if (a - regs[i].va < regs[i].len) return regs[i].huge;
    return 0;
}

static uint64_t nat_pa_of(const void *p) {
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) return ~0ull;
    uint64_t e = 0, va = (uint64_t)(uintptr_t)p;
    ssize_t n = pread(fd, &e, sizeof e, (off_t)(va / 4096 * 8));
    close(fd);
    if (n != (ssize_t)sizeof e || !(e >> 63)) return ~0ull;
    uint64_t pfn = e & ((1ull << 55) - 1);
    if (!pfn) return ~0ull; /* no CAP_SYS_ADMIN: PFNs read as zero */
    return pfn * 4096 + (va & 4095);
}

static const uint8_t *nat_shared_buf(size_t bytes) {
    size_t len = (bytes + G.page - 1) / G.page * G.page;
    int fd = memfd_create("shd-lab2-shared", MFD_CLOEXEC);
    if (fd < 0) hx__die("memfd_create: %s", strerror(errno));
    if (ftruncate(fd, (off_t)len) != 0) hx__die("ftruncate: %s", strerror(errno));
    uint8_t *w = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (w == MAP_FAILED) hx__die("mmap memfd: %s", strerror(errno));
    for (size_t i = 0; i < len; i++) w[i] = (uint8_t)(i * 131 + 7);
    munmap(w, len);
    const uint8_t *r = mmap(NULL, len, PROT_READ, MAP_SHARED | MAP_POPULATE, fd, 0);
    if (r == MAP_FAILED) hx__die("mmap memfd ro: %s", strerror(errno));
    close(fd);
    return r;
}

/* ------------------------------------------------------------------ timing ops */
static void nat_access(const void *p) { shd_access(p); }
static uint8_t nat_read8(const void *p) { return *(const volatile uint8_t *)p; }
static uint64_t nat_probe(const void *p) { return shd_time_access(p); }
static void nat_flush(const void *p) { shd_flush(p); }
static void nat_fence(void) { shd_mfence(); }
static uint64_t nat_now(void) { return shd_now(); }
static void nat_wait(uint64_t ticks) {
    uint64_t t0 = shd_now();
    if (yield_mode) {
        do sched_yield();
        while (shd_now() - t0 < ticks);
    } else {
        while (shd_now() - t0 < ticks) { }
    }
}

/* ------------------------------------------------------------------ processes */
static void kill_kids(void) {
    if (is_child) return;
    for (int i = 0; i < nkids; i++)
        if (kids[i].pid > 0) { kill(kids[i].pid, SIGKILL); waitpid(kids[i].pid, NULL, 0); kids[i].pid = 0; }
}
static void on_alarm(int sig) {
    (void)sig;
    static const char m[] = "RESULT FAIL timeout (wall-clock limit)\n";
    if (write(1, m, sizeof m - 1) < 0) { /* ignore */ }
    for (int i = 0; i < nkids; i++)
        if (kids[i].pid > 0) kill(kids[i].pid, SIGKILL);
    _exit(3);
}
static void on_term(int sig) {
    for (int i = 0; i < nkids; i++)
        if (kids[i].pid > 0) kill(kids[i].pid, SIGKILL);
    signal(sig, SIG_DFL);
    raise(sig);
}

static int nat_spawn(void (*fn)(void *), void *arg, hx_place_t where, const char *name) {
    if (nkids >= 16) hx__die("too many parties");
    int cpu = cpu_main;
    if (where == HX_SIBLING) cpu = cpu_sib >= 0 ? cpu_sib : cpu_main;
    if (where == HX_OTHER_CORE) {
        cpu = cpu_other;
        if (cpu < 0) {
            fprintf(stderr, "[native] warning: only one usable CPU; '%s' shares it with main\n", name);
            cpu = cpu_main;
        }
    }
    if (cpu == cpu_main) yield_mode = 1; /* time-sliced sharing: wait() must yield */
    fprintf(stderr, "[native] party '%s' -> cpu %d%s\n", name, cpu,
            (where == HX_SIBLING && cpu_sib < 0) ? " (no SMT sibling: same CPU, time-sliced)" : "");
    int pfd[2];
    if (pipe(pfd) != 0) hx__die("pipe: %s", strerror(errno));
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) hx__die("fork: %s", strerror(errno));
    if (pid == 0) {
        is_child = 1;
        nkids = 0;
        close(pfd[0]);
        ret_fd = pfd[1];
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        shd_pin_cpu(cpu);
        fn(arg);
        _exit(0);
    }
    close(pfd[1]);
    kids[nkids] = (child_t){pid, pfd[0]};
    return ++nkids; /* ids start at 1 */
}

static void nat_ret(const void *data, size_t len) {
    if (!is_child) hx__die("hx_return() called from main");
    const uint8_t *d = data;
    while (len) {
        ssize_t n = write(ret_fd, d, len);
        if (n <= 0) break;
        d += n;
        len -= (size_t)n;
    }
    _exit(0);
}

static int nat_join(int id, char *out, size_t cap) {
    if (id < 1 || id > nkids) return -1;
    child_t *k = &kids[id - 1];
    size_t got = 0;
    for (;;) {
        char tmp[4096];
        ssize_t n = read(k->rfd, tmp, sizeof tmp);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        size_t take = (size_t)n;
        if (got + take > cap - 1) take = cap - 1 - got;
        memcpy(out + got, tmp, take);
        got += take;
    }
    out[got] = 0;
    waitpid(k->pid, NULL, 0);
    k->pid = 0;
    close(k->rfd);
    return (int)got;
}

static void nat_trace_start(void) { }
static int nat_trace_stop(uint64_t *lines, int cap) { (void)lines; (void)cap; return -1; }
static void nat_set_timeout(unsigned wall, uint64_t vt) {
    (void)vt;
    if (wall) { signal(SIGALRM, on_alarm); alarm(wall); }
}

static const plat_geom_t *nat_geom(void) { return &G; }

static void nat_init(uint64_t seed) {
    (void)seed;
    shd_timer_init();
    choose_cpus();
    read_geometry();
    if (shd_pin_cpu(cpu_main) != 0) fprintf(stderr, "[native] warning: cannot pin to cpu %d\n", cpu_main);
    atexit(kill_kids);
    signal(SIGINT, on_term);
    signal(SIGTERM, on_term);
    fprintf(stderr, "[native] arch=%s timer=%s main cpu=%d sibling=%d other=%d  L1 %dx%d  L2 %dx%d  LLC %dx%d\n",
#if defined(__x86_64__)
            "x86_64",
#else
            "aarch64",
#endif
            shd_timer_name(), cpu_main, cpu_sib, cpu_other, G.l1_sets, G.l1_ways, G.l2_sets, G.l2_ways,
            G.llc_sets, G.llc_ways);
}

const backend_t native_backend = {
    .name = "native",
    .init = nat_init,
    .geom = nat_geom,
    .alloc = nat_alloc,
    .is_huge = nat_is_huge,
    .access = nat_access,
    .read8 = nat_read8,
    .probe = nat_probe,
    .flush = nat_flush,
    .fence = nat_fence,
    .now = nat_now,
    .wait = nat_wait,
    .spawn = nat_spawn,
    .ret = nat_ret,
    .join = nat_join,
    .shared_buf = nat_shared_buf,
    .pa_of = nat_pa_of,
    .trace_start = nat_trace_start,
    .trace_stop = nat_trace_stop,
    .set_timeout = nat_set_timeout,
};
