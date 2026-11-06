/* collector_frame.h - everything a trace collector needs EXCEPT the measurement itself.
 *
 * A collector file defines COLLECTOR_NAME, includes this header, and implements
 *
 *     static uint64_t measure_window(const collector_ctx *c, uint64_t deadline_ns);
 *
 * which must keep working until fp_now_ns() >= deadline_ns and return "how much work got done"
 * in that window (number of complete buffer sweeps for the sweep-counting collector, number of
 * loop iterations for the counter collector of Part 3).
 *
 * Command line (all optional):
 *   -P <ms>        window length P in milliseconds            (default 10)
 *   -T <s>         trace length T in seconds                  (default 2)
 *   -b <bytes>     sweep buffer size; suffixes K/M accepted   (default: LLC size from sysfs, see below)
 *   -o <file>      output file                                (default: stdout)
 *   --cpu <n>      pin to CPU n (-1 = do not pin)             (default -1)
 *   --wait-start   print "ready" on stdout after setup, then wait for one line on stdin before
 *                  recording (tools/collect.py uses this to start victim and collector together)
 *
 * Output ("fp-trace v1"): one header line starting with '#', then one integer per window.
 */
#ifndef COLLECTOR_FRAME_H
#define COLLECTOR_FRAME_H
#include "fp.h"

#ifndef COLLECTOR_NAME
#error "define COLLECTOR_NAME before including collector_frame.h"
#endif

/* The default buffer is the LLC size, but clamped: server parts report huge shared LLCs
 * (the x86 cloud box says 260 MiB) and one sweep of such a buffer would take longer than a
 * window. Override with -b. */
#define FP_BUF_FALLBACK (8u << 20)
#define FP_BUF_MIN (256u << 10)
#define FP_BUF_MAX (8u << 20)

typedef struct {
    volatile uint8_t *buf; /* sweep buffer (page-aligned, pre-faulted) */
    size_t bytes;          /* buffer size in bytes */
    size_t line;           /* cache-line size in bytes (stride for one access per line) */
    uint64_t period_ns;    /* window length P */
} collector_ctx;

static uint64_t measure_window(const collector_ctx *c, uint64_t deadline_ns);

static size_t fp__parse_bytes(const char *s) {
    char *end;
    double v = strtod(s, &end);
    if (*end == 'K' || *end == 'k') v *= 1024;
    else if (*end == 'M' || *end == 'm') v *= 1024 * 1024;
    else if (*end == 'G' || *end == 'g') v *= 1024 * 1024 * 1024;
    return (size_t)v;
}

int main(int argc, char **argv) {
    double period_ms = 10, trace_s = 2;
    size_t bytes = 0;
    const char *out = NULL;
    int cpu = -1, wait_start = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-P") && i + 1 < argc) period_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) trace_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) bytes = fp__parse_bytes(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wait-start")) wait_start = 1;
        else fp_die("usage: %s [-P ms] [-T s] [-b bytes] [-o file] [--cpu n] [--wait-start]", argv[0]);
    }
    if (period_ms <= 0 || trace_s <= 0) fp_die("P and T must be positive");
    size_t llc = fp_llc_bytes();
    if (bytes == 0) {
        bytes = llc ? llc : FP_BUF_FALLBACK;
        if (bytes > FP_BUF_MAX) bytes = FP_BUF_MAX;
        if (bytes < FP_BUF_MIN) bytes = FP_BUF_MIN;
    }
    size_t windows = (size_t)(trace_s * 1000.0 / period_ms + 0.5);
    if (windows == 0) windows = 1;

    fp_pin(cpu);
    collector_ctx c;
    c.line = (size_t)shd_line_size();
    bytes = (bytes + c.line - 1) / c.line * c.line;
    c.bytes = bytes;
    c.buf = shd_alloc(bytes, 0);
    c.period_ns = (uint64_t)(period_ms * 1e6);
    uint64_t *counts = calloc(windows, sizeof *counts);
    if (!counts) fp_die("out of memory");

    /* Warm up caches, TLB and clock frequency for ~100 ms; results discarded. */
    uint64_t t = fp_now_ns();
    for (int i = 0; i < 10; i++) (void)measure_window(&c, t += 10000000ull);

    if (wait_start) {
        char line[16];
        printf("ready\n");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) fp_die("collector: no start signal");
    }

    /* Window w covers [t0 + w*P, t0 + (w+1)*P). Deadlines are absolute, so a late window
     * (e.g. we were descheduled) does not shift every later window. */
    uint64_t t0 = fp_now_ns();
    for (size_t w = 0; w < windows; w++) counts[w] = measure_window(&c, t0 + (w + 1) * c.period_ns);
    uint64_t elapsed = fp_now_ns() - t0;

    FILE *f = out ? fopen(out, "w") : stdout;
    if (!f) fp_die("cannot open %s", out);
    fprintf(f, "# fp-trace v1 collector=%s P_ms=%g T_s=%g windows=%zu buf=%zu line=%zu llc=%zu cpu=%d elapsed_ms=%.1f\n",
            COLLECTOR_NAME, period_ms, trace_s, windows, bytes, c.line, llc, cpu, elapsed / 1e6);
    for (size_t w = 0; w < windows; w++) fprintf(f, "%llu\n", (unsigned long long)counts[w]);
    if (out) fclose(f);
    free(counts);
    return 0;
}

#endif
