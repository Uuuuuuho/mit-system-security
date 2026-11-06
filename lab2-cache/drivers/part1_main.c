/* part1_main.c - driver for Part 1 (latency histograms and thresholds). Do not edit.
 *
 *   ./bin/part1 [--seed N] [--samples N] [--csv FILE]
 *
 * Calls your measure_level() for every level, prints histograms, calls your calibrate() and, in sim
 * mode, grades the result against the simulator's ground-truth latency distributions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "lab.h"
#include "sim_oracle.h"

static const char *lvl_name[5] = {"", "L1", "L2", "LLC", "DRAM"};

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static uint64_t pct(const uint64_t *sorted, int n, double p) {
    int i = (int)(p * (n - 1));
    return sorted[i];
}

int main(int argc, char **argv) {
    hx_init(argc, argv, "part1");
    hx_set_timeout(120, 0);
    int n = hx_arg("--samples") ? atoi(hx_arg("--samples")) : 2000;
    if (n < 100) n = 100;
    uint64_t *s[5] = {0}, *sorted[5] = {0};
    for (int l = 1; l <= 4; l++) {
        s[l] = calloc((size_t)n, sizeof(uint64_t));
        sorted[l] = calloc((size_t)n, sizeof(uint64_t));
        measure_level(l, s[l], n);
        memcpy(sorted[l], s[l], sizeof(uint64_t) * (size_t)n);
        qsort(sorted[l], (size_t)n, sizeof(uint64_t), cmp_u64);
    }

    printf("\n%-5s %8s %8s %8s %8s %8s   (ticks, %d samples each)\n", "level", "min", "p10", "median", "p90", "max", n);
    for (int l = 1; l <= 4; l++)
        printf("%-5s %8llu %8llu %8llu %8llu %8llu\n", lvl_name[l], (unsigned long long)sorted[l][0],
               (unsigned long long)pct(sorted[l], n, .10), (unsigned long long)pct(sorted[l], n, .5),
               (unsigned long long)pct(sorted[l], n, .90), (unsigned long long)sorted[l][n - 1]);

    /* one combined text histogram over [0, p99 of DRAM] */
    uint64_t hi = 0;
    for (int l = 1; l <= 4; l++) { uint64_t v = pct(sorted[l], n, .99); if (v > hi) hi = v; }
    if (hi < 8) hi = 8;
    enum { NB = 32 };
    uint64_t w = (hi + NB - 1) / NB;
    printf("\nhistogram (bin width %llu ticks; each column = count of samples of that level)\n", (unsigned long long)w);
    printf("%13s %6s %6s %6s %6s\n", "bin", "L1", "L2", "LLC", "DRAM");
    for (int b = 0; b < NB; b++) {
        int c[5] = {0};
        for (int l = 1; l <= 4; l++)
            for (int i = 0; i < n; i++)
                if (s[l][i] / w == (uint64_t)b) c[l]++;
        if (c[1] + c[2] + c[3] + c[4] == 0) continue;
        printf("[%5llu,%5llu) %6d %6d %6d %6d\n", (unsigned long long)(b * w), (unsigned long long)((b + 1) * w), c[1],
               c[2], c[3], c[4]);
    }

    const char *csv = hx_arg("--csv");
    if (csv) {
        FILE *f = fopen(csv, "w");
        if (f) {
            fprintf(f, "level,latency\n");
            for (int l = 1; l <= 4; l++)
                for (int i = 0; i < n; i++) fprintf(f, "%s,%llu\n", lvl_name[l], (unsigned long long)s[l][i]);
            fclose(f);
            printf("wrote %s\n", csv);
        } else {
            perror(csv);
        }
    }

    cal_t c = calibrate();
    printf("\ncalibrate(): med L1=%llu L2=%llu LLC=%llu DRAM=%llu  l2_miss=%llu  llc_miss=%llu\n",
           (unsigned long long)c.med[1], (unsigned long long)c.med[2], (unsigned long long)c.med[3],
           (unsigned long long)c.med[4], (unsigned long long)c.l2_miss, (unsigned long long)c.llc_miss);

    if (!plat_is_sim()) {
        int ok = c.med[1] < c.med[3] && c.med[3] < c.med[4] && c.l2_miss < c.llc_miss;
        hx_result(ok, "native run (not graded against ground truth; check ordering only)");
    }

    /* ---------------- grading against the model's true distributions ---------------- */
    enum { M = 4000 };
    static uint64_t truth[5][M];
    int pass = 1;
    char why[512] = "";
    for (int l = 1; l <= 4; l++) {
        for (int i = 0; i < M; i++) truth[l][i] = sim_oracle_sample(l);
        qsort(truth[l], M, sizeof(uint64_t), cmp_u64);
        uint64_t lo = truth[l][M / 50], up = truth[l][M - M / 50];
        uint64_t med = pct(sorted[l], n, .5);
        if (med < lo || med > up) {
            pass = 0;
            snprintf(why + strlen(why), sizeof why - strlen(why), " %s-median %llu not in [%llu,%llu];", lvl_name[l],
                     (unsigned long long)med, (unsigned long long)lo, (unsigned long long)up);
        }
        if (c.med[l] < lo || c.med[l] > up) {
            pass = 0;
            snprintf(why + strlen(why), sizeof why - strlen(why), " cal.med[%s] off;", lvl_name[l]);
        }
    }
    /* threshold accuracy on fresh ground-truth samples */
    int ok2 = 0, ok3 = 0;
    for (int i = 0; i < M; i++) {
        ok2 += truth[1][i] <= c.l2_miss; ok2 += truth[2][i] <= c.l2_miss;
        ok2 += truth[3][i] > c.l2_miss;  ok2 += truth[4][i] > c.l2_miss;
        ok3 += truth[1][i] <= c.llc_miss; ok3 += truth[2][i] <= c.llc_miss;
        ok3 += truth[3][i] <= c.llc_miss; ok3 += truth[4][i] > c.llc_miss;
    }
    double a2 = ok2 / (4.0 * M), a3 = ok3 / (4.0 * M);
    printf("threshold accuracy on ground truth: l2_miss %.2f%%  llc_miss %.2f%%\n", 100 * a2, 100 * a3);
    if (a2 < 0.97) { pass = 0; strcat(why, " l2_miss threshold misclassifies;"); }
    if (a3 < 0.97) { pass = 0; strcat(why, " llc_miss threshold misclassifies;"); }
    hx_result(pass, "l2_miss=%llu llc_miss=%llu%s", (unsigned long long)c.l2_miss, (unsigned long long)c.llc_miss,
              pass ? "" : why);
}
