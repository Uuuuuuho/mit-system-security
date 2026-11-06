/* sim.c - the deterministic "sim" backend.
 *
 * Pieces:
 *   1. Machine configuration (geometry, latencies, noise), overridable with SHD_SIM_* variables.
 *   2. Physical memory: every 4 KiB page you allocate gets a random physical frame; every 2 MiB
 *      "huge" page gets a random 2 MiB-aligned physical chunk. So with small pages only the low 12
 *      address bits are known to user code; with huge pages the low 21 bits are.
 *   3. Parties = cooperative coroutines (ucontext). Each party has its own virtual clock. The
 *      scheduler always runs the party whose clock is smallest and switches when the running party
 *      gets more than SHD_SIM_QUANTUM cycles ahead of the next one or calls plat_wait(). This
 *      approximates parties running in parallel (SMT siblings / other cores) in virtual time.
 *   4. Timing model: plat_probe returns timer overhead + level latency + jitter (+ rare outliers).
 *   5. Background noise: one "OS" coroutine per core touches random lines now and then and
 *      occasionally pollutes many lines at once (a context switch / interrupt storm).
 * Given the same seed, a run is bit-for-bit reproducible.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "backend.h"
#include "sim_cache.h"
#include "sim_oracle.h"

/* ------------------------------------------------------------------ 1. configuration */
static struct {
    int l1_sets, l1_ways, l2_sets, l2_ways, llc_sets, llc_ways;
    int repl_l1, repl_l2, repl_llc;
    int lat[5];          /* [1]=L1 [2]=L2 [3]=LLC [4]=DRAM base latency in cycles */
    int timer_overhead;  /* cost of reading the timer around a probe            */
    double outlier_p;    /* probability that a probe is hit by an "interrupt"     */
    int noise;           /* 0 = off, 1 = default, 2/3 = heavier                    */
    int quantum;
    int nohuge;
    int stats;
    hier_opts_t opt;
} cfg;

static plat_geom_t geom;
static hier_t H;
static rng_t rng_lat, rng_mem, rng_noise;

static int env_int(const char *name, int dflt) {
    const char *s = getenv(name);
    return (s && *s) ? atoi(s) : dflt;
}
static int parse_repl(const char *s, int dflt) {
    if (!s || !*s) return dflt;
    if (!strcmp(s, "lru")) return REPL_LRU;
    if (!strcmp(s, "plru")) return REPL_PLRU;
    if (!strcmp(s, "random")) return REPL_RANDOM;
    hx__die("SHD_SIM_REPL must be lru, plru or random");
}

/* ------------------------------------------------------------------ 2. physical memory */
#define SMALL_FRAMES (1u << 21) /* 8 GiB of 4 KiB frames for small pages            */
#define HUGE_CHUNKS 4096u       /* 8 GiB more, as 2 MiB chunks, starting at 8 GiB  */
#define HUGE_BASE (8ull << 30)
#define HUGE_SZ (2ull << 20)
static uint8_t *frame_used;    /* bitmap */
static uint8_t chunk_used[HUGE_CHUNKS];

typedef struct {
    uintptr_t va;
    size_t len;
    int huge;
    uint32_t *pfn;     /* small pages: one frame per 4 KiB    */
    uint64_t *chunk;   /* huge pages: PA of each 2 MiB chunk  */
} region_t;
#define MAX_REGIONS 512
static region_t regions[MAX_REGIONS];
static int nregions;
static region_t *last_region;

/* pages touched through plat_* that were not allocated with plat_alloc (stack, malloc, ...) */
#define FB_CAP (1u << 16)
static uint64_t fb_vpn[FB_CAP], fb_pfn[FB_CAP];
static unsigned fb_count;

static uint32_t new_frame(void) {
    for (;;) {
        uint32_t f = (uint32_t)(rng_next(&rng_mem) % SMALL_FRAMES);
        if (f < 16) continue;
        if (!(frame_used[f >> 3] & (1u << (f & 7)))) {
            frame_used[f >> 3] |= (uint8_t)(1u << (f & 7));
            return f;
        }
    }
}
static uint64_t new_chunk(void) {
    for (int tries = 0; tries < 100000; tries++) {
        uint32_t c = (uint32_t)(rng_next(&rng_mem) % HUGE_CHUNKS);
        if (!chunk_used[c]) { chunk_used[c] = 1; return HUGE_BASE + (uint64_t)c * HUGE_SZ; }
    }
    hx__die("sim: out of simulated huge pages");
}

static uint64_t fallback_pa(uintptr_t a) {
    uint64_t vpn = a >> 12, h = (vpn * 0x9E3779B97F4A7C15ULL) >> 48;
    for (unsigned i = 0; i < FB_CAP; i++) {
        unsigned k = (unsigned)((h + i) & (FB_CAP - 1));
        if (fb_vpn[k] == vpn + 1) return (fb_pfn[k] << 12) | (a & 4095);
        if (fb_vpn[k] == 0) {
            if (++fb_count > FB_CAP / 2) hx__die("sim: too many un-allocated pages touched");
            fb_vpn[k] = vpn + 1;
            fb_pfn[k] = new_frame();
            return (fb_pfn[k] << 12) | (a & 4095);
        }
    }
    hx__die("sim: page table full");
}

static inline uint64_t sim_pa(const void *p) {
    uintptr_t a = (uintptr_t)p;
    region_t *r = last_region;
    if (!r || a - r->va >= r->len) {
        r = NULL;
        for (int i = 0; i < nregions; i++)
            if (a - regions[i].va < regions[i].len) { r = &regions[i]; break; }
        if (!r) return fallback_pa(a);
        last_region = r;
    }
    uintptr_t off = a - r->va;
    if (r->huge) return r->chunk[off >> 21] + (off & (HUGE_SZ - 1));
    return ((uint64_t)r->pfn[off >> 12] << 12) | (off & 4095);
}

static void *sim_alloc(size_t bytes, int huge) {
    if (nregions >= MAX_REGIONS) hx__die("sim: too many allocations");
    if (cfg.nohuge) huge = 0;
    size_t unit = huge ? HUGE_SZ : 4096;
    size_t len = (bytes + unit - 1) / unit * unit;
    if (len == 0) len = unit;
    /* host memory (lazily backed); for huge regions align the host VA to 2 MiB so VA[20:0]==PA[20:0] */
    size_t maplen = len + (huge ? HUGE_SZ : 0);
    uint8_t *m = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) hx__die("sim: mmap of %zu bytes failed", maplen);
    uintptr_t va = (uintptr_t)m;
    if (huge) va = (va + HUGE_SZ - 1) & ~(uintptr_t)(HUGE_SZ - 1);
    region_t *r = &regions[nregions++];
    memset(r, 0, sizeof *r);
    r->va = va;
    r->len = len;
    r->huge = huge;
    if (huge) {
        r->chunk = malloc(sizeof(uint64_t) * (len / HUGE_SZ));
        for (size_t i = 0; i < len / HUGE_SZ; i++) r->chunk[i] = new_chunk();
    } else {
        r->pfn = malloc(sizeof(uint32_t) * (len / 4096));
        for (size_t i = 0; i < len / 4096; i++) r->pfn[i] = new_frame();
    }
    return (void *)va;
}

static int sim_is_huge(const void *p) {
    uintptr_t a = (uintptr_t)p;
    for (int i = 0; i < nregions; i++)
        if (a - regions[i].va < regions[i].len) return regions[i].huge;
    return 0;
}

/* ------------------------------------------------------------------ 3. parties / scheduler */
enum { T_FREE = 0, T_RUN, T_JOIN, T_DONE };
typedef struct {
    ucontext_t ctx;
    uint64_t clock;
    int core, hart, state, join_target;
    void (*fn)(void *);
    void *arg;
    char name[24];
    char *ret;
    size_t retlen;
} party_t;

#define MAX_PARTIES 12
#define STACK_SZ (8u << 20)
static party_t P[MAX_PARTIES];
static int nparties, cur;
static uint64_t g_limit = UINT64_MAX, g_vlimit = UINT64_MAX;
static uint64_t n_switches;

static int trace_on, trace_owner, trace_n, trace_cap;
static uint64_t *trace_buf;

static void recompute_limit(void) {
    uint64_t m = UINT64_MAX;
    for (int i = 0; i < nparties; i++)
        if (i != cur && P[i].state == T_RUN && P[i].clock < m) m = P[i].clock;
    g_limit = (m == UINT64_MAX) ? UINT64_MAX : m + (uint64_t)cfg.quantum;
}

static int pick(void) {
    int best = -1;
    for (int i = 0; i < nparties; i++)
        if (P[i].state == T_RUN && (best < 0 || P[i].clock < P[best].clock)) best = i;
    return best;
}

static void switch_to(int nxt, int save) {
    int prev = cur;
    cur = nxt;
    recompute_limit();
    n_switches++;
    if (save) swapcontext(&P[prev].ctx, &P[nxt].ctx);
    else setcontext(&P[nxt].ctx);
}

static void sim_yield(void) {
    int nxt = pick();
    if (nxt < 0) hx__die("sim: deadlock (no runnable party)");
    if (nxt == cur) { recompute_limit(); return; }
    switch_to(nxt, 1);
}

static void timeout_virtual(void) {
    fflush(NULL);
    printf("RESULT FAIL timeout (virtual time limit of %llu ticks exceeded in party '%s')\n",
           (unsigned long long)g_vlimit, P[cur].name);
    fflush(stdout);
    _exit(3);
}

static inline void charge(uint64_t c) {
    P[cur].clock += c;
    if (P[cur].clock > g_limit) {
        if (P[cur].clock > g_vlimit) timeout_virtual();
        sim_yield();
    }
}

static void party_exit(void) {
    party_t *me = &P[cur];
    me->state = T_DONE;
    for (int i = 0; i < nparties; i++)
        if (P[i].state == T_JOIN && P[i].join_target == cur) {
            P[i].state = T_RUN;
            if (P[i].clock < me->clock) P[i].clock = me->clock;
        }
    int nxt = pick();
    if (nxt < 0) hx__die("sim: deadlock after '%s' finished", me->name);
    switch_to(nxt, 0);
    __builtin_unreachable();
}

static void trampoline(void) {
    party_t *me = &P[cur];
    me->fn(me->arg);
    party_exit();
}

static int new_party(void (*fn)(void *), void *arg, int core, int hart, const char *name) {
    if (nparties >= MAX_PARTIES) hx__die("sim: too many parties");
    int id = nparties++;
    party_t *t = &P[id];
    memset(t, 0, sizeof *t);
    t->clock = P[cur].clock;
    t->core = core;
    t->hart = hart;
    t->state = T_RUN;
    t->fn = fn;
    t->arg = arg;
    snprintf(t->name, sizeof t->name, "%s", name ? name : "party");
    void *stk = mmap(NULL, STACK_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (stk == MAP_FAILED) hx__die("sim: cannot allocate a party stack");
    getcontext(&t->ctx);
    t->ctx.uc_stack.ss_sp = stk;
    t->ctx.uc_stack.ss_size = STACK_SZ;
    t->ctx.uc_link = NULL;
    makecontext(&t->ctx, trampoline, 0);
    recompute_limit();
    return id;
}

static int sim_spawn(void (*fn)(void *), void *arg, hx_place_t where, const char *name) {
    int core = where == HX_OTHER_CORE ? 1 : 0;
    int hart = where == HX_SIBLING ? 1 : 0;
    return new_party(fn, arg, core, hart, name);
}

static void sim_ret(const void *data, size_t len) {
    party_t *me = &P[cur];
    if (cur == 0) hx__die("hx_return() called from the main party");
    me->ret = malloc(len + 1);
    memcpy(me->ret, data, len);
    me->ret[len] = 0;
    me->retlen = len;
    party_exit();
}

static int sim_join(int id, char *out, size_t cap) {
    if (id <= 0 || id >= nparties) return -1;
    if (P[id].state != T_DONE) {
        P[cur].state = T_JOIN;
        P[cur].join_target = id;
        sim_yield();
    }
    if (!P[id].ret) { if (cap) out[0] = 0; return 0; }
    size_t n = P[id].retlen < cap - 1 ? P[id].retlen : cap - 1;
    memcpy(out, P[id].ret, n);
    out[n] = 0;
    return (int)n;
}

/* ------------------------------------------------------------------ 4. timing model */
static inline int core_of_cur(void) { return P[cur].core; }

static uint64_t level_latency(int lvl, rng_t *r) {
    uint64_t lat = (uint64_t)cfg.lat[lvl];
    if (lvl == LVL_HIT_DRAM) {
        /* DRAM: row-buffer hit/miss mix plus queueing jitter */
        lat += rng_next(r) % 25 + rng_next(r) % 25;
        if (rng_next(r) % 4 == 0) lat += 35;
    } else if (lvl == LVL_HIT_LLC) {
        lat += rng_next(r) % 7; /* ring hops to the slice */
    }
    return lat;
}
static int64_t timer_jitter(rng_t *r) { return (int64_t)(rng_next(r) % 4) + (int64_t)(rng_next(r) % 4) - 3; }

static inline int do_access(const void *p) {
    uint64_t pa = sim_pa(p);
    if (trace_on && cur == trace_owner && trace_n < trace_cap) trace_buf[trace_n++] = pa >> 6;
    return hier_access(&H, P[cur].core, P[cur].hart, pa);
}

static void sim_access(const void *p) {
    int lvl = do_access(p);
    charge(level_latency(lvl, &rng_lat));
}
static uint8_t sim_read8(const void *p) {
    sim_access(p);
    return *(const volatile uint8_t *)p;
}
static uint64_t sim_probe(const void *p) {
    int lvl = do_access(p);
    int64_t m = (int64_t)cfg.timer_overhead + (int64_t)level_latency(lvl, &rng_lat) + timer_jitter(&rng_lat);
    if (rng_unit(&rng_lat) < cfg.outlier_p) m += 300 + (int64_t)(rng_next(&rng_lat) % 3000);
    if (m < 1) m = 1;
    charge((uint64_t)m);
    return (uint64_t)m;
}
static void sim_flush(const void *p) {
    hier_flush(&H, sim_pa(p));
    charge(30);
}
static void sim_fence(void) { charge(8); }
static uint64_t sim_now(void) {
    charge(18);
    return P[cur].clock;
}
static void sim_wait(uint64_t ticks) {
    P[cur].clock += ticks;
    if (P[cur].clock > g_vlimit) timeout_virtual();
    sim_yield();
}

/* oracle hooks used only by graders (sim_oracle.h) */
uint64_t sim_oracle_sample(int level) {
    static rng_t r;
    static int init;
    if (!init) { rng_seed(&r, 0xabcdef); init = 1; }
    int64_t m = (int64_t)cfg.timer_overhead + (int64_t)level_latency(level, &r) + timer_jitter(&r);
    return (uint64_t)(m < 1 ? 1 : m);
}
int sim_oracle_where(const void *p) { return hier_where(&H, core_of_cur(), sim_pa(p)); }

/* ------------------------------------------------------------------ 5. background noise */
static void noise_party(void *arg) {
    int core = (int)(intptr_t)arg;
    size_t len = 4u << 20;
    uint8_t *buf = sim_alloc(len, 0);
    uint64_t nlines = len / 64;
    int lvl = cfg.noise;
    uint64_t next_burst = P[cur].clock + 200000 + rng_next(&rng_noise) % 800000;
    (void)core;
    for (;;) {
        int k = 1 + (int)(rng_next(&rng_noise) % (uint64_t)(2 * lvl + 1));
        for (int i = 0; i < k; i++) sim_access(buf + (rng_next(&rng_noise) % nlines) * 64);
        sim_wait(1500 + rng_next(&rng_noise) % (uint64_t)(8000 / lvl));
        if (P[cur].clock >= next_burst) {
            /* "context switch": many random lines at once */
            int n = 600 * lvl + (int)(rng_next(&rng_noise) % 600);
            for (int i = 0; i < n; i++) sim_access(buf + (rng_next(&rng_noise) % nlines) * 64);
            next_burst = P[cur].clock + 400000 + rng_next(&rng_noise) % (uint64_t)(1600000 / lvl);
        }
    }
}

/* ------------------------------------------------------------------ misc ops */
static const uint8_t *sim_shared_buf(size_t bytes) {
    uint8_t *b = sim_alloc(bytes, 0);
    for (size_t i = 0; i < bytes; i++) b[i] = (uint8_t)(i * 131 + 7);
    return b;
}
static uint64_t sim_pa_of(const void *p) { return sim_pa(p); }
static void sim_trace_start(void) {
    trace_cap = 1 << 16;
    if (!trace_buf) trace_buf = malloc(sizeof(uint64_t) * (size_t)trace_cap);
    trace_n = 0;
    trace_owner = cur;
    trace_on = 1;
}
static int sim_trace_stop(uint64_t *lines, int cap) {
    trace_on = 0;
    int n = trace_n < cap ? trace_n : cap;
    memcpy(lines, trace_buf, sizeof(uint64_t) * (size_t)n);
    return n;
}

static void on_alarm(int sig) {
    (void)sig;
    static const char m[] = "RESULT FAIL timeout (wall-clock limit)\n";
    if (write(1, m, sizeof m - 1) < 0) { /* nothing */ }
    _exit(3);
}
static void sim_set_timeout(unsigned wall, uint64_t vt) {
    if (wall) { signal(SIGALRM, on_alarm); alarm(wall); }
    if (vt) g_vlimit = P[cur].clock + vt;
}

static void print_stats(void) {
    if (!cfg.stats) return;
    fprintf(stderr, "[sim] accesses L1=%llu L2=%llu LLC=%llu DRAM=%llu switches=%llu main_clock=%llu\n",
            (unsigned long long)H.stat_access[1], (unsigned long long)H.stat_access[2],
            (unsigned long long)H.stat_access[3], (unsigned long long)H.stat_access[4],
            (unsigned long long)n_switches, (unsigned long long)P[0].clock);
}

static const plat_geom_t *sim_geom(void) { return &geom; }

static void sim_init(uint64_t seed) {
    cfg.l1_sets = 64; cfg.l1_ways = 8;         /* 32 KiB */
    cfg.l2_sets = 1024; cfg.l2_ways = 16;      /* 1 MiB  */
    cfg.llc_sets = 4096; cfg.llc_ways = 16;    /* 4 MiB  */
    int r = parse_repl(getenv("SHD_SIM_REPL"), -1);
    cfg.repl_l1 = r >= 0 ? r : REPL_PLRU;
    cfg.repl_l2 = r >= 0 ? r : REPL_PLRU;
    cfg.repl_llc = r >= 0 ? r : REPL_LRU;
    cfg.lat[1] = 4; cfg.lat[2] = 14; cfg.lat[3] = 42; cfg.lat[4] = 170;
    cfg.timer_overhead = 24;
    cfg.outlier_p = 0.002;
    cfg.noise = env_int("SHD_SIM_NOISE", 1);
    if (cfg.noise < 0) cfg.noise = 0;
    if (cfg.noise > 3) cfg.noise = 3;
    cfg.quantum = env_int("SHD_SIM_QUANTUM", 1000);
    if (cfg.quantum < 50) cfg.quantum = 50;
    cfg.nohuge = env_int("SHD_SIM_NOHUGE", 0);
    cfg.stats = env_int("SHD_SIM_STATS", 0);
    cfg.opt.stride_pf = env_int("SHD_SIM_PREFETCH", 1);
    cfg.opt.adjacent_pf = cfg.opt.stride_pf;
    cfg.opt.partition = env_int("SHD_SIM_PARTITION", 0);

    geom.line = 64;
    geom.l1_sets = cfg.l1_sets; geom.l1_ways = cfg.l1_ways;
    geom.l2_sets = cfg.l2_sets; geom.l2_ways = cfg.l2_ways;
    geom.llc_sets = cfg.llc_sets; geom.llc_ways = cfg.llc_ways;
    geom.page = 4096;
    geom.huge = HUGE_SZ;

    rng_seed(&rng_lat, seed ^ 0x1a7e9c11u);
    rng_seed(&rng_mem, seed ^ 0x9e3779b9u);
    rng_seed(&rng_noise, seed ^ 0x0153a11eu);
    frame_used = calloc(SMALL_FRAMES / 8, 1);
    hier_init(&H, 2, cfg.l1_sets, cfg.l1_ways, cfg.repl_l1, cfg.l2_sets, cfg.l2_ways, cfg.repl_l2, cfg.llc_sets,
              cfg.llc_ways, cfg.repl_llc, cfg.opt, seed);

    /* party 0 = main (attacker), core 0 hart 0, running on the original stack */
    nparties = 1;
    cur = 0;
    memset(&P[0], 0, sizeof P[0]);
    P[0].state = T_RUN;
    snprintf(P[0].name, sizeof P[0].name, "main");
    P[0].clock = 1000000 + rng_next(&rng_mem) % 1000000;
    if (cfg.noise > 0) {
        new_party(noise_party, (void *)(intptr_t)0, 0, 2, "os-noise-0");
        new_party(noise_party, (void *)(intptr_t)1, 1, 2, "os-noise-1");
    }
    g_vlimit = (uint64_t)50e9;
    atexit(print_stats);
    if (cfg.opt.partition) fprintf(stderr, "[sim] L1/L2 way-partitioning between harts is ON\n");
}

const backend_t sim_backend = {
    .name = "sim",
    .init = sim_init,
    .geom = sim_geom,
    .alloc = sim_alloc,
    .is_huge = sim_is_huge,
    .access = sim_access,
    .read8 = sim_read8,
    .probe = sim_probe,
    .flush = sim_flush,
    .fence = sim_fence,
    .now = sim_now,
    .wait = sim_wait,
    .spawn = sim_spawn,
    .ret = sim_ret,
    .join = sim_join,
    .shared_buf = sim_shared_buf,
    .pa_of = sim_pa_of,
    .trace_start = sim_trace_start,
    .trace_stop = sim_trace_stop,
    .set_timeout = sim_set_timeout,
};
