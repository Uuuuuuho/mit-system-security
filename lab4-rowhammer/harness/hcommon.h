/* hcommon.h - shared helpers for the lab4 harness programs (provided; not part of the exercises). */
#ifndef LAB4_HCOMMON_H
#define LAB4_HCOMMON_H
#include <stdint.h>

#include "../dramsim/dramsim.h"
#include "../dramsim/dramsim_oracle.h"
#include "../include/lab4.h"

typedef struct {
    uint64_t seed;
    int verbose;
    int reveal;
    const char *mode; /* part 1: auto | native | sim */
} hargs_t;

typedef struct {
    int part;
    int passed, total;
} hres_t;

void hargs_parse(int argc, char **argv, hargs_t *a, const char *usage);
void hcheck(hres_t *r, int ok, const char *name, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
int hfinish(hres_t *r); /* prints the RESULT line; returns the process exit code */

uint64_t hrand(uint64_t *state);
dsim_t *hsim(uint64_t seed, int trr);
void hmap_from_oracle(const dsim_t *s, dram_map_t *m);
void hprint_mask(uint64_t mask); /* DRAMA notation: a13^a18 */

/* The harness's own hammer (used by Parts 5 and 6 so they do not depend on Part 4). It uses the
 * oracle mapping. href_fill writes `pattern` to every row in [lo, hi] that is not an aggressor
 * and ~pattern to the aggressors; href_hammer accesses the aggressors round-robin for `ns`. */
void href_fill(dsim_t *s, unsigned bank, unsigned lo, unsigned hi, const unsigned *aggr, int n, uint64_t pattern);
void href_hammer(dsim_t *s, unsigned bank, const unsigned *aggr, int n, double ns);
/* rows [first, first+count) of every bank lie in the buffer */
void hbuf_rows(const dsim_t *s, unsigned *first, unsigned *count);

#endif
