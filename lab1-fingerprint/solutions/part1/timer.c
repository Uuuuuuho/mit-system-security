/* timer.c - Part 1: how many memory accesses does a timer need before it can tell them apart?
 * REFERENCE SOLUTION.
 *
 * We time N dependent loads ("pointer chasing") in two rings:
 *   hot  : a 16 KiB ring that stays in the L1 data cache       (~1 ns per load)
 *   cold : a large ring (default 64 MiB) in random line order   (misses L1/L2, often the LLC)
 * with three clocks:
 *   native : clock_gettime(CLOCK_MONOTONIC)          (ns units; real resolution 1-42 ns)
 *   0.1 ms : the same, rounded down to 100 us         (like performance.now() in many browsers)
 *   1 ms   : rounded down to 1 ms                     (like the most conservative browser settings)
 * For each N we repeat the hot and cold measurements R times and ask: if an attacker sees ONE
 * measurement, how often can a single threshold tell hot from cold? N is "measurable" for a
 * clock when that single-shot accuracy reaches 90%.
 *
 *   ./timer [--quick] [--max-n N] [--big-mib M] [--selftest]
 */
#include "fp.h"

#include <math.h>

#define SMALL_BYTES (16u << 10)
#define NCLOCKS 3
static const uint64_t clock_res_ns[NCLOCKS] = {0, 100000, 1000000};
static const char *clock_name[NCLOCKS] = {"native", "0.1 ms", "1 ms"};
#define MEASURABLE 0.90

/* -------------------------------------------------------------------------------------------
 * Exercise 1-1 (a): a coarse clock. Round the timestamp t_ns DOWN to a multiple of res_ns.
 * res_ns == 0 means "no rounding" (return t_ns unchanged).
 */
static uint64_t coarsen(uint64_t t_ns, uint64_t res_ns) {
    if (res_ns == 0) return t_ns;
    return t_ns - t_ns % res_ns;
}

/* -------------------------------------------------------------------------------------------
 * Exercise 1-1 (b): time n dependent loads.
 * ring[i] holds the index of the next element. Start at *pos, follow the ring n times, write the
 * final index back to *pos, and store raw fp_now_ns() timestamps taken right before and right
 * after the loop in *t0 and *t1.
 * Pitfalls: the compiler must not drop or move the loop (use its result: *pos), and every load
 * must depend on the previous one (that is what makes it a latency measurement).
 */
static void timed_chase(const uint32_t *ring, uint32_t *pos, long n, uint64_t *t0, uint64_t *t1) {
    uint32_t p = *pos;
    *t0 = fp_now_ns();
    for (long i = 0; i < n; i++) p = ring[p];
    *t1 = fp_now_ns();
    *pos = p;
}

/* -------------------------------------------------------------------------------------------
 * Exercise 1-1 (c): single-shot distinguishability.
 * An attacker gets one duration d and guesses "cold" if d > thr, else "hot". Return the BEST
 * accuracy over all thresholds thr, i.e. max over thr of
 *     (#hot samples with d <= thr  +  #cold samples with d > thr) / (2n).
 * The result is always >= 0.5 (pick thr above every sample).
 */
static double single_shot_accuracy(const double *hot, const double *cold, int n) {
    double best = 0.5;
    for (int c = 0; c < 2 * n; c++) {
        double thr = c < n ? hot[c] : cold[c - n];
        int ok = 0;
        for (int i = 0; i < n; i++) ok += (hot[i] <= thr) + (cold[i] > thr);
        double acc = (double)ok / (2.0 * n);
        if (acc > best) best = acc;
    }
    return best;
}

/* =========================== provided code below this line =========================== */

/* Ring over one element per cache line, visiting lines in random order (Sattolo's algorithm
 * produces a single cycle, so the chase never gets stuck in a short loop). */
static uint32_t *make_ring(size_t bytes, size_t line, uint64_t *rng) {
    size_t per_line = line / sizeof(uint32_t), n = bytes / line;
    uint32_t *ring = shd_alloc(bytes, 0);
    uint32_t *perm = malloc(n * sizeof *perm);
    if (!perm) fp_die("out of memory");
    for (size_t i = 0; i < n; i++) perm[i] = (uint32_t)i;
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = fp_rand(rng) % i;
        uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    for (size_t i = 0; i < n; i++) ring[perm[i] * per_line] = (uint32_t)(perm[(i + 1) % n] * per_line);
    free(perm);
    return ring;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double median(const double *v, int n) {
    double *t = malloc(n * sizeof *t);
    memcpy(t, v, n * sizeof *t);
    qsort(t, n, sizeof *t, cmp_double);
    double m = t[n / 2];
    free(t);
    return m;
}

static int selftest(void) {
    int bad = 0;
#define EXPECT(cond)                                                       \
    do {                                                                   \
        if (!(cond)) { printf("selftest: FAIL  %s\n", #cond); bad++; }     \
    } while (0)
    EXPECT(coarsen(123456789, 100000) == 123400000);
    EXPECT(coarsen(123456789, 1000000) == 123000000);
    EXPECT(coarsen(999999, 1000000) == 0);
    EXPECT(coarsen(5000000, 1000000) == 5000000);
    EXPECT(coarsen(42, 0) == 42);
    double h1[] = {1, 2, 3}, c1[] = {4, 5, 6};
    double h2[] = {1, 1, 1}, c2[] = {1, 1, 1};
    double h3[] = {1, 2, 5}, c3[] = {3, 4, 6};
    EXPECT(fabs(single_shot_accuracy(h1, c1, 3) - 1.0) < 1e-9);
    EXPECT(fabs(single_shot_accuracy(h2, c2, 3) - 0.5) < 1e-9);
    EXPECT(fabs(single_shot_accuracy(h3, c3, 3) - 5.0 / 6.0) < 1e-9);
    /* timed_chase must follow the ring and report a sane interval */
    uint32_t ring[4] = {2, 0, 3, 1}, pos = 0;
    uint64_t t0 = 0, t1 = 0;
    timed_chase(ring, &pos, 3, &t0, &t1); /* 0 -> 2 -> 3 -> 1 */
    EXPECT(pos == 1);
    EXPECT(t0 > 0 && t1 >= t0 && t1 - t0 < 1000000000ull);
    printf("selftest: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}

int main(int argc, char **argv) {
    int quick = 0;
    long max_n = 10000000;
    size_t big_mib = 64;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--quick")) quick = 1, max_n = 1000000;
        else if (!strcmp(argv[i], "--max-n") && i + 1 < argc) max_n = atol(argv[++i]);
        else if (!strcmp(argv[i], "--big-mib") && i + 1 < argc) big_mib = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--selftest")) return selftest();
        else fp_die("usage: %s [--quick] [--max-n N] [--big-mib M] [--selftest]", argv[0]);
    }
    uint64_t rng = fp_now_ns();
    size_t line = (size_t)shd_line_size();
    uint32_t *hot = make_ring(SMALL_BYTES, line, &rng);
    uint32_t *cold = make_ring(big_mib << 20, line, &rng);
    uint32_t hpos = 0, cpos = 0;
    struct timespec res;
    clock_getres(CLOCK_MONOTONIC, &res);
    printf("hot ring %u KiB (L1-resident), cold ring %zu MiB in random line order, %zu B lines\n",
           SMALL_BYTES >> 10, big_mib, line);
    printf("clock_gettime(CLOCK_MONOTONIC) reported resolution: %ld ns\n\n", res.tv_nsec);
    printf("%9s %5s |", "", "");
    for (int k = 0; k < NCLOCKS; k++) printf(" %-28s |", clock_name[k]);
    printf("\n%9s %5s |", "N", "reps");
    for (int k = 0; k < NCLOCKS; k++) printf(" %9s %11s %5s |", "hot(us)", "cold(us)", "acc");
    printf("    (hot/cold = median duration, acc = single-shot accuracy)\n");

    long first_ok[NCLOCKS] = {-1, -1, -1};
    const double budget_ns = quick ? 0.25e9 : 1.0e9; /* per N, for the cold measurements */
    double cold_ns_per_load = 100.0;                   /* refined as we go */
    enum { RMAX = 201, RMIN = 9 };
    static double d[NCLOCKS][2][RMAX];
    for (int e = 0;; e++) {
        long n = (long)llround(pow(10.0, e / 2.0)); /* 1, 3, 10, 32, 100, ... */
        if (n > max_n) break;
        int reps = (int)(budget_ns / (cold_ns_per_load * (double)n + 2000.0));
        if (reps > RMAX) reps = RMAX;
        if (reps < RMIN) reps = RMIN;
        double cold_total = 0;
        for (int r = 0; r < reps; r++) {
            uint64_t t[2][2];
            timed_chase(hot, &hpos, 256, &t[0][0], &t[0][1]); /* re-warm the hot ring */
            int first = (int)(fp_rand(&rng) & 1);             /* random order within a pair */
            for (int s = 0; s < 2; s++) {
                int which = s ^ first; /* 0 = hot, 1 = cold */
                if (which == 0) timed_chase(hot, &hpos, n, &t[0][0], &t[0][1]);
                else timed_chase(cold, &cpos, n, &t[1][0], &t[1][1]);
            }
            cold_total += (double)(t[1][1] - t[1][0]);
            for (int k = 0; k < NCLOCKS; k++)
                for (int w = 0; w < 2; w++)
                    d[k][w][r] = (double)(coarsen(t[w][1], clock_res_ns[k]) - coarsen(t[w][0], clock_res_ns[k]));
        }
        if (n >= 1000) cold_ns_per_load = cold_total / reps / (double)n;
        printf("%9ld %5d |", n, reps);
        for (int k = 0; k < NCLOCKS; k++) {
            double acc = single_shot_accuracy(d[k][0], d[k][1], reps);
            if (acc >= MEASURABLE && first_ok[k] < 0) first_ok[k] = n;
            printf(" %9.3f %11.3f %5.2f |", median(d[k][0], reps) / 1e3, median(d[k][1], reps) / 1e3, acc);
        }
        printf("\n");
        fflush(stdout);
    }
    printf("\nsmallest N with single-shot accuracy >= %.2f:", MEASURABLE);
    for (int k = 0; k < NCLOCKS; k++) {
        if (first_ok[k] < 0) printf("  %s=none", clock_name[k]);
        else printf("  %s=%ld", clock_name[k], first_ok[k]);
    }
    printf("\nmeasurable: native=%ld coarse_0.1ms=%ld coarse_1ms=%ld\n", first_ok[0], first_ok[1], first_ok[2]);
    printf("(cold ring: about %.1f ns per dependent load on this machine)\n", cold_ns_per_load);
    return hpos == 0xffffffffu && cpos == 0xffffffffu; /* keeps the chases "used" */
}
