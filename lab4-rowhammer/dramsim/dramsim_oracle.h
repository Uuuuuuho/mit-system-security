/* dramsim_oracle.h - ground truth of a dramsim instance. FOR THE GRADER AND HARNESS ONLY.
 *
 * Student code must not include this header: everything it exposes is what Parts 2-4 ask you to
 * discover from timing and from bit flips.
 */
#ifndef DRAMSIM_ORACLE_H
#define DRAMSIM_ORACLE_H
#include "dramsim.h"

typedef struct {
    int nfns;            /* number of bank-address functions (log2 nbanks) */
    uint64_t fn[8];      /* the functions, full width (may include bits above the buffer) */
    int pivot[8];        /* a bit that occurs in fn[i] and in no other function (below row_lo) */
    int row_lo;          /* lowest row bit; row = (pa >> row_lo) & (rows_per_bank - 1) */
    int addr_bits;       /* log2(mem_bytes) */
    int buf_bits;        /* log2(buf_size): bits [0, buf_bits) vary inside the buffer */
    uint64_t col_mask;   /* address bits that form the column index (13 bits) */
    int template_id;     /* which family of mappings was drawn (0, 1, 2) */
    const char *template_name;
    int cand_true;       /* index of the real set in dsim_candidates() */
    int trr_entries;     /* in-DRAM TRR sampler entries per bank */
    int trr_every;       /* TRR acts on every trr_every-th REF */
    int trr_threshold;   /* minimum sampler count before TRR refreshes the neighbours */
    double w2;           /* coupling weight at distance 2 */
    double f_same;       /* disturbance factor when aggressor and victim bit are equal */
    double hc_median;    /* median of the per-row threshold above MAC */
    unsigned orient_block;
} dsim_truth_t;

void dsim_oracle_truth(const dsim_t *s, dsim_truth_t *t);
void dsim_oracle_decode(const dsim_t *s, uint64_t pa, unsigned *bank, unsigned *row, unsigned *col);
uint64_t dsim_oracle_compose(const dsim_t *s, unsigned bank, unsigned row, unsigned col);
/* weakest-cell threshold of a row and its orientation (1 = true cell: flips 1->0) */
double dsim_oracle_row_threshold(const dsim_t *s, unsigned bank, unsigned row);
int dsim_oracle_row_orientation(const dsim_t *s, unsigned bank, unsigned row);
/* restrict a mask to the address bits that vary inside the buffer (and above the line offset) */
uint64_t dsim_oracle_restrict(const dsim_t *s, uint64_t mask);

#endif
