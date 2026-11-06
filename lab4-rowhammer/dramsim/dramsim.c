/* dramsim.c - implementation of the lab4 DRAM model. See dramsim.h for the public API and
 * README.md ("The simulated module") for the model in prose.
 *
 * Reading this file tells you HOW secrets are drawn, not WHAT they are for your seed: the seed is
 * fresh on every `make check`. Still, do the lab from timing and flips first; that is the point.
 *
 * Model summary
 *   Address mapping   bank bit i = parity(pa & fn[i]); row = pa >> row_lo; column = the 13
 *                     remaining low bits. Each fn[i] has a private "pivot" bit below row_lo.
 *   Timing            open-page controller; hit = tCL, closed = tRCD+tCL, conflict = tRP+tRCD+tCL,
 *                     ACT-to-ACT in one bank >= tRC, plus a fixed path overhead, Gaussian noise,
 *                     rare interrupt-like spikes, and background traffic that occasionally opens
 *                     a different row in a random bank.
 *   Refresh           one REF every tREFI (stalls the module for tRFC and closes all rows);
 *                     REF number j refreshes rows [k*32, k*32+32) of every bank, k = j mod 1024.
 *   Disturbance       activating row A adds weight 1 (distance 1) or w2 (distance 2) times a data
 *                     factor (1 if the aggressor bit differs from the victim bit, f_same otherwise)
 *                     to every weak cell of the neighbour rows that currently holds its charged
 *                     value. A cell flips when its accumulator reaches its threshold. A row's
 *                     accumulators are cleared when it is refreshed or activated.
 *   TRR (optional)    per-bank sampler table of E (row, count) entries with LRU replacement; a
 *                     newly inserted row starts at count 1. On every trr_every-th REF the TRR picks
 *                     one entry whose count reached the threshold (at random), refreshes that
 *                     row's two neighbours, and then clears the bank's table.
 */
#include "dramsim.h"
#include "dramsim_oracle.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROW_BYTES     8192u
#define ROW_WORDS     (ROW_BYTES / 8u)
#define ROWS_PER_BANK 32768u
#define REFS_PER_WIN  1024u
#define ROWS_PER_REF  (ROWS_PER_BANK / REFS_PER_WIN)
#define BUF_BITS      30
#define MAX_FNS       8
#define MAX_WEAK      24
#define MAC_FLOOR     12000.0
#define W2_MAX        0.25
#define TWO_PI        6.283185307179586

/* ------------------------------------------------------------------ helpers */
static inline uint64_t mix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
typedef struct { uint64_t s; } rng_t;
static inline uint64_t rng_next(rng_t *r) { r->s += 0x9E3779B97F4A7C15ull; return mix64(r->s); }
static inline double rng_unit(rng_t *r) { return (double)(rng_next(r) >> 11) * (1.0 / 9007199254740992.0); }
static inline unsigned rng_below(rng_t *r, unsigned n) { return (unsigned)(rng_next(r) % n); }
static double rng_normal(rng_t *r) {
    double u1 = rng_unit(r), u2 = rng_unit(r);
    if (u1 < 1e-300) u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(TWO_PI * u2);
}
static inline int parity64(uint64_t x) { return __builtin_parityll(x); }

/* GF(2) rank of a set of masks */
static int gf2_rank(const uint64_t *in, int n) {
    uint64_t v[64];
    int r = 0;
    for (int i = 0; i < n && i < 64; i++) v[i] = in[i];
    for (int bit = 63; bit >= 0 && r < n; bit--) {
        int p = -1;
        for (int i = r; i < n; i++) if ((v[i] >> bit) & 1) { p = i; break; }
        if (p < 0) continue;
        uint64_t t = v[p]; v[p] = v[r]; v[r] = t;
        for (int i = 0; i < n; i++) if (i != r && ((v[i] >> bit) & 1)) v[i] ^= v[r];
        r++;
    }
    return r;
}

/* ------------------------------------------------------------------ state */
typedef struct { float thr; uint16_t word; uint8_t bit; } weak_t;
typedef struct {
    double epoch;         /* accumulators hold disturbance received since this time */
    uint8_t n;            /* number of weak cells */
    uint8_t orient;       /* charged value of this row's cells: 1 = true cell, 0 = anti cell */
    weak_t cell[MAX_WEAK];
    float acc[MAX_WEAK];
} rowstate_t;

struct dsim {
    dsim_info_t info;
    dsim_opts_t opts;
    uint64_t secret;
    /* mapping */
    int nfns, row_lo, addr_bits, template_id;
    uint64_t fn[MAX_FNS];
    int pivot[MAX_FNS];
    uint64_t col_mask;
    int col_pos[13];
    dsim_fnset_t cands[3];
    int cand_true;
    /* timing, ns */
    double tCL, tRCD, tRP, tRAS, tRC, tRFC, tREFI, tREFW, overhead, cpu_ns, ghz, sigma_cyc, spike_p, bg_p;
    /* disturbance */
    double w2, f_same, hc_median, hc_sigma;
    unsigned orient_block;
    /* TRR */
    int trr_entries, trr_every, trr_threshold;
    uint32_t *trr_row, *trr_cnt;
    uint64_t *trr_stamp, trr_clock;
    uint8_t *trr_valid;
    rng_t trr_rng;
    /* dynamic state */
    double now, t_first_ref, next_ref, ref_busy_until, pending_ns;
    uint64_t ref_index;
    int32_t *open_row;
    double *last_act;
    rowstate_t **rs;
    uint64_t **data;
    rng_t noise;
    dsim_stats_t st;
    dsim_mitigation_t mit;
    int has_mit;
    int warned_range, warned_align;
};

/* ------------------------------------------------------------------ mapping generation */
static const char *template_names[3] = {"ddr3-classic", "ddr4-bankgroup", "hashed"};

/* Draw one set of bank functions from a template family. Pivots are unique per function and
 * lie below row_lo, so the functions are always linearly independent (also inside the buffer). */
static void gen_mapping(rng_t *r, int nfns, int addr_bits, int tmpl, uint64_t fn[], int pivot[]) {
    int row_lo = 13 + nfns;
    int used[8] = {0};
    for (int i = 0; i < nfns; i++) pivot[i] = 13 + i;          /* template 0 */
    if (tmpl == 1) pivot[0] = 7;                                /* BG0-like low bit */
    if (tmpl == 2) pivot[0] = 8 + (int)rng_below(r, 2);         /* anchor of a wide hash */
    if (tmpl != 0) for (int i = 1; i < nfns; i++) pivot[i] = 13 + i;

    for (int i = 0; i < nfns; i++) fn[i] = 1ull << pivot[i];

    if (tmpl == 0) {
        /* each pivot XOR one of the lowest row bits (Sandy/Ivy-Bridge-like) */
        int perm[8];
        for (int i = 0; i < nfns; i++) perm[i] = i;
        for (int i = nfns - 1; i > 0; i--) { int j = (int)rng_below(r, (unsigned)i + 1); int t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
        int shift = (int)rng_below(r, 3);
        int alone = rng_unit(r) < 0.3 ? (int)rng_below(r, (unsigned)nfns) : -1; /* rank-like bit */
        for (int i = 0; i < nfns; i++)
            if (i != alone) fn[i] |= 1ull << (row_lo + shift + perm[i]);
    } else {
        /* distinct partners among the 8 lowest row bits; one function gets a second partner */
        for (int i = (tmpl == 2 ? 1 : 0); i < nfns; i++) {
            int k;
            do k = (int)rng_below(r, 8); while (used[k]);
            used[k] = 1;
            fn[i] |= 1ull << (row_lo + k);
        }
        if (tmpl == 1) {
            int i = 1 + (int)rng_below(r, (unsigned)nfns - 1), k;
            do k = (int)rng_below(r, 8); while (used[k]);
            used[k] = 1;
            fn[i] |= 1ull << (row_lo + k);
        }
        if (tmpl == 2) {
            /* channel-hash-like: pivot ^ two column bits ^ 4-6 row bits from the whole row range */
            int ncol = 0;
            while (ncol < 2) {
                int c = 6 + (int)rng_below(r, 7);
                if (c == pivot[0] || ((fn[0] >> c) & 1)) continue;
                fn[0] |= 1ull << c; ncol++;
            }
            int nrow = 4 + (int)rng_below(r, 3), got = 0, low = 0;
            while (got < nrow) {
                int b = row_lo + (int)rng_below(r, (unsigned)(addr_bits - row_lo));
                if ((fn[0] >> b) & 1) continue;
                if (got >= nrow - 2 && low < 2 && b >= BUF_BITS) continue; /* keep >=2 visible */
                fn[0] |= 1ull << b; got++;
                if (b < BUF_BITS) low++;
            }
        }
    }
}

static uint64_t restrict_mask(uint64_t m) { return m & ((1ull << BUF_BITS) - 1) & ~0x3Full; }

static int span_equal_restricted(const uint64_t *a, int na, const uint64_t *b, int nb) {
    uint64_t all[16];
    int n = 0;
    for (int i = 0; i < na; i++) all[n++] = restrict_mask(a[i]);
    int ra = gf2_rank(all, n);
    for (int i = 0; i < nb; i++) all[n++] = restrict_mask(b[i]);
    int rb = gf2_rank(all + na, nb), rab = gf2_rank(all, n);
    return ra == rb && rab == ra;
}

/* ------------------------------------------------------------------ rows */
static void init_rowstate(const dsim_t *s, unsigned bank, unsigned row, rowstate_t *rs) {
    rng_t h = {mix64(s->secret ^ ((uint64_t)bank * 0x100000001B3ull) ^ ((uint64_t)row << 21) ^ 0x5157ull)};
    double z = rng_normal(&h);
    double hc = MAC_FLOOR + exp(log(s->hc_median) + s->hc_sigma * z);
    rs->orient = (uint8_t)(mix64(s->secret ^ 0xA11CEull ^ ((uint64_t)bank * 7919u) ^
                                 ((uint64_t)(row / s->orient_block) << 32)) & 1);
    rs->n = 0;
    int base = 2 + (int)rng_below(&h, 7);
    for (int k = 0; k < base && rs->n < MAX_WEAK; k++) {
        unsigned word = rng_below(&h, ROW_WORDS), bit = rng_below(&h, 64);
        double thr = (k == 0) ? hc : hc * (1.0 - 0.6 * log(1.0 - rng_unit(&h)));
        int extra = rng_unit(&h) < 0.3 ? 1 + (int)rng_below(&h, 2) : 0; /* clustered weak cells */
        for (int e = 0; e <= extra && rs->n < MAX_WEAK; e++) {
            unsigned b = (e == 0) ? bit : rng_below(&h, 64);
            double t = (e == 0) ? thr : thr * (1.0 + 0.15 * rng_unit(&h));
            int dup = 0;
            for (int q = 0; q < rs->n; q++) if (rs->cell[q].word == word && rs->cell[q].bit == b) dup = 1;
            if (dup) continue;
            rs->cell[rs->n].word = (uint16_t)word;
            rs->cell[rs->n].bit = (uint8_t)b;
            rs->cell[rs->n].thr = (float)t;
            rs->acc[rs->n] = 0.0f;
            rs->n++;
        }
    }
}

static inline size_t gidx(unsigned bank, unsigned row) { return (size_t)bank * ROWS_PER_BANK + row; }

static rowstate_t *get_rs(dsim_t *s, unsigned bank, unsigned row, double t) {
    size_t g = gidx(bank, row);
    rowstate_t *r = s->rs[g];
    if (!r) {
        r = (rowstate_t *)malloc(sizeof *r);
        if (!r) { perror("dramsim: malloc"); exit(1); }
        init_rowstate(s, bank, row, r);
        r->epoch = t;
        s->rs[g] = r;
    }
    return r;
}

static uint64_t *get_data(dsim_t *s, size_t g) {
    if (!s->data[g]) {
        s->data[g] = (uint64_t *)calloc(ROW_WORDS, sizeof(uint64_t));
        if (!s->data[g]) { perror("dramsim: calloc"); exit(1); }
    }
    return s->data[g];
}

/* time of the most recent periodic refresh of `row` at or before t (-1 if none yet) */
static double last_periodic(const dsim_t *s, unsigned row, double t) {
    if (t < s->t_first_ref) return -1.0;
    long long jmax = (long long)floor((t - s->t_first_ref) / s->tREFI);
    long long slot = row / ROWS_PER_REF;
    if (jmax < slot) return -1.0;
    long long j = jmax - ((jmax - slot) % REFS_PER_WIN);
    return s->t_first_ref + (double)j * s->tREFI;
}

static void disturb(dsim_t *s, unsigned bank, unsigned victim, unsigned aggr, double w, double t) {
    rowstate_t *v = get_rs(s, bank, victim, t);
    double lr = last_periodic(s, victim, t);
    if (lr > v->epoch) {
        for (int k = 0; k < v->n; k++) v->acc[k] = 0.0f;
        v->epoch = lr;
    }
    size_t gv = gidx(bank, victim);
    uint64_t *vd = s->data[gv];
    const uint64_t *ad = s->data[gidx(bank, aggr)];
    for (int k = 0; k < v->n; k++) {
        const weak_t *c = &v->cell[k];
        int vb = vd ? (int)((vd[c->word] >> c->bit) & 1) : 0;
        if (vb != v->orient) continue; /* discharged cells cannot flip */
        int ab = ad ? (int)((ad[c->word] >> c->bit) & 1) : 0;
        v->acc[k] += (float)(w * (ab != vb ? 1.0 : s->f_same));
        if (v->acc[k] >= c->thr) {
            vd = get_data(s, gv);
            vd[c->word] ^= 1ull << c->bit;
            v->acc[k] = 0.0f;
            s->st.flips++;
        }
    }
}

static void trr_sample(dsim_t *s, unsigned bank, unsigned row) {
    int E = s->trr_entries;
    uint32_t *rw = s->trr_row + (size_t)bank * E, *cn = s->trr_cnt + (size_t)bank * E;
    uint64_t *ts = s->trr_stamp + (size_t)bank * E;
    uint8_t *va = s->trr_valid + (size_t)bank * E;
    int freei = -1, lru = -1;
    s->trr_clock++;
    for (int i = 0; i < E; i++) {
        if (va[i] && rw[i] == row) { cn[i]++; ts[i] = s->trr_clock; return; }
        if (!va[i]) { if (freei < 0) freei = i; }
        else if (lru < 0 || ts[i] < ts[lru]) lru = i;
    }
    int i = freei >= 0 ? freei : lru; /* the table is full: forget the least recently seen row */
    rw[i] = row; cn[i] = 1; ts[i] = s->trr_clock; va[i] = 1;
}

static void activate(dsim_t *s, unsigned bank, unsigned row, double t, int external);

static void internal_refresh(dsim_t *s, unsigned bank, long row, double t) {
    if (row < 0 || row >= (long)ROWS_PER_BANK) return;
    activate(s, bank, (unsigned)row, t, 0); /* a refresh is an activation: restores the row and
                                               disturbs its neighbours (Half-Double!) */
}

static void activate(dsim_t *s, unsigned bank, unsigned row, double t, int external) {
    rowstate_t *self = s->rs[gidx(bank, row)];
    if (self) {
        for (int k = 0; k < self->n; k++) self->acc[k] = 0.0f;
        self->epoch = t;
    }
    for (int d = -2; d <= 2; d++) {
        if (d == 0) continue;
        long v = (long)row + d;
        if (v < 0 || v >= (long)ROWS_PER_BANK) continue;
        disturb(s, bank, (unsigned)v, row, (d == 1 || d == -1) ? 1.0 : s->w2, t);
    }
    if (!external) return;
    s->st.acts++;
    if (s->opts.trr) trr_sample(s, bank, row);
    if (s->has_mit && s->mit.on_activate) s->mit.on_activate(s->mit.state, s, bank, row);
}

static void trr_event(dsim_t *s, double t) {
    int E = s->trr_entries;
    for (unsigned b = 0; b < s->info.nbanks; b++) {
        uint32_t *rw = s->trr_row + (size_t)b * E, *cn = s->trr_cnt + (size_t)b * E;
        uint8_t *va = s->trr_valid + (size_t)b * E;
        int cand[MAX_FNS * 2], nc = 0;
        for (int i = 0; i < E; i++)
            if (va[i] && cn[i] >= (uint32_t)s->trr_threshold) cand[nc++] = i;
        for (int i = 0; i < E; i++) va[i] = 0; /* the sampler starts over after every TRR REF */
        if (nc == 0) continue;
        long r = rw[cand[rng_below(&s->trr_rng, (unsigned)nc)]];
        for (int d = -1; d <= 1; d += 2) {
            if (r + d < 0 || r + d >= (long)ROWS_PER_BANK) continue;
            internal_refresh(s, b, r + d, t);
            s->st.trr_refreshes++;
        }
    }
}

static void do_ref(dsim_t *s) {
    double t = s->next_ref;
    uint64_t j = s->ref_index++;
    s->next_ref += s->tREFI;
    s->ref_busy_until = t + s->tRFC;
    s->st.refs++;
    for (unsigned b = 0; b < s->info.nbanks; b++) s->open_row[b] = -1;
    if (j % REFS_PER_WIN == 0 && s->has_mit && s->mit.on_window) s->mit.on_window(s->mit.state, s);
    if (s->opts.trr && j % (uint64_t)s->trr_every == 0) trr_event(s, t);
}

/* ------------------------------------------------------------------ decode / compose */
static inline void decode(const dsim_t *s, uint64_t pa, unsigned *bank, unsigned *row, unsigned *col) {
    unsigned b = 0;
    for (int i = 0; i < s->nfns; i++) b |= (unsigned)parity64(pa & s->fn[i]) << i;
    *bank = b;
    *row = (unsigned)((pa >> s->row_lo) & (ROWS_PER_BANK - 1));
    unsigned c = 0;
    for (int j = 0; j < 13; j++) c |= (unsigned)((pa >> s->col_pos[j]) & 1) << j;
    *col = c;
}

static uint64_t compose(const dsim_t *s, unsigned bank, unsigned row, unsigned col) {
    uint64_t pa = (uint64_t)row << s->row_lo;
    for (int j = 0; j < 13; j++) if ((col >> j) & 1) pa |= 1ull << s->col_pos[j];
    for (int i = 0; i < s->nfns; i++)
        if (parity64(pa & s->fn[i]) != (int)((bank >> i) & 1)) pa ^= 1ull << s->pivot[i];
    return pa;
}

/* ------------------------------------------------------------------ access path */
static double noise_gauss(dsim_t *s) {
    /* Irwin-Hall approximation: cheap and good enough for latency noise */
    double u = rng_unit(&s->noise) + rng_unit(&s->noise) + rng_unit(&s->noise) + rng_unit(&s->noise);
    return (u - 2.0) * 1.7320508075688772;
}

static int do_access(dsim_t *s, uint64_t pa, uint32_t *lat_out, size_t *g_out, unsigned *col_out) {
    s->st.accesses++;
    if (pa < s->info.buf_base || pa >= s->info.buf_base + s->info.buf_size) {
        s->st.violations++;
        if (!s->warned_range) {
            fprintf(stderr, "dramsim: access to 0x%llx outside your buffer ignored (further warnings suppressed)\n",
                    (unsigned long long)pa);
            s->warned_range = 1;
        }
        if (lat_out) *lat_out = 0;
        return 0;
    }
    while (s->now >= s->next_ref) do_ref(s);
    double stall = 0.0;
    if (s->now < s->ref_busy_until) { stall = s->ref_busy_until - s->now; s->now = s->ref_busy_until; }

    /* background traffic from "other cores": sometimes another row gets opened in some bank */
    if (s->opts.noise > 0.0 && rng_unit(&s->noise) < s->bg_p * s->opts.noise)
        s->open_row[rng_below(&s->noise, s->info.nbanks)] = (int32_t)rng_below(&s->noise, ROWS_PER_BANK);

    unsigned bank, row, col;
    decode(s, pa, &bank, &row, &col);
    double lat;
    if (s->open_row[bank] == (int32_t)row) {
        lat = s->tCL;
        s->st.row_hits++;
    } else {
        double pre = 0.0;
        if (s->open_row[bank] >= 0) { pre = s->tRP; s->st.row_conflicts++; }
        else s->st.row_closed++;
        double t_act = s->now + pre;
        if (t_act < s->last_act[bank] + s->tRC) t_act = s->last_act[bank] + s->tRC;
        lat = (t_act - s->now) + s->tRCD + s->tCL;
        s->last_act[bank] = t_act;
        s->open_row[bank] = (int32_t)row;
        double before = s->pending_ns;
        activate(s, bank, row, t_act, 1);
        if (s->pending_ns > before) s->open_row[bank] = -1; /* the mitigation used the bank */
    }
    double ns = stall + lat + s->overhead;
    double cyc = ns * s->ghz;
    double extra_ns = 0.0;
    if (s->opts.noise > 0.0) {
        cyc += s->sigma_cyc * s->opts.noise * noise_gauss(s);
        if (rng_unit(&s->noise) < s->spike_p * s->opts.noise) {
            double sp = 200.0 + 2800.0 * rng_unit(&s->noise); /* interrupt-like outlier */
            cyc += sp;
            extra_ns = sp / s->ghz;
        }
    }
    s->now += ns + s->cpu_ns + s->pending_ns + extra_ns;
    s->pending_ns = 0.0;
    if (cyc < 1.0) cyc = 1.0;
    if (lat_out) *lat_out = (uint32_t)(cyc + 0.5);
    if (g_out) *g_out = gidx(bank, row);
    if (col_out) *col_out = col;
    return 1;
}

uint32_t dsim_access(dsim_t *s, uint64_t pa) {
    uint32_t lat;
    do_access(s, pa, &lat, NULL, NULL);
    return lat;
}

static void check_align(dsim_t *s, uint64_t pa) {
    if ((pa & 7) && !s->warned_align) {
        fprintf(stderr, "dramsim: unaligned 64-bit access at 0x%llx (rounded down)\n", (unsigned long long)pa);
        s->warned_align = 1;
    }
}

uint64_t dsim_read64(dsim_t *s, uint64_t pa) {
    size_t g;
    unsigned col;
    check_align(s, pa);
    if (!do_access(s, pa, NULL, &g, &col)) return 0;
    return s->data[g] ? s->data[g][col >> 3] : 0;
}

void dsim_write64(dsim_t *s, uint64_t pa, uint64_t value) {
    size_t g;
    unsigned col;
    check_align(s, pa);
    if (!do_access(s, pa, NULL, &g, &col)) return;
    get_data(s, g)[col >> 3] = value;
}

void dsim_idle(dsim_t *s, double ns) { if (ns > 0) s->now += ns; }
double dsim_now_ns(const dsim_t *s) { return s->now; }
void dsim_get_stats(const dsim_t *s, dsim_stats_t *out) { *out = s->st; }
const dsim_info_t *dsim_info(const dsim_t *s) { return &s->info; }
void dsim_candidates(const dsim_t *s, dsim_fnset_t out[3]) { memcpy(out, s->cands, sizeof s->cands); }

void dsim_set_mitigation(dsim_t *s, const dsim_mitigation_t *m) {
    if (m) { s->mit = *m; s->has_mit = 1; }
    else { memset(&s->mit, 0, sizeof s->mit); s->has_mit = 0; }
}

void dsim_mc_refresh(dsim_t *s, unsigned bank, unsigned row) {
    if (bank >= s->info.nbanks || row >= ROWS_PER_BANK) return;
    internal_refresh(s, bank, (long)row, s->now);
    s->st.mc_refreshes++;
    s->pending_ns += s->tRC;
}

/* ------------------------------------------------------------------ create / destroy */
dsim_t *dsim_create(const dsim_opts_t *o) {
    dsim_t *s = (dsim_t *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->opts = *o;
    s->secret = mix64(o->seed ^ 0xD1B54A32D192ED03ull);
    rng_t r = {s->secret};

    /* geometry: 1 channel, 1 or 2 ranks, 16 banks per rank, 32768 rows of 8 KiB per bank */
    s->nfns = 4 + (int)rng_below(&r, 2);
    s->row_lo = 13 + s->nfns;
    s->addr_bits = s->row_lo + 15;
    s->template_id = (int)rng_below(&r, 3);
    gen_mapping(&r, s->nfns, s->addr_bits, s->template_id, s->fn, s->pivot);
    uint64_t piv = 0;
    for (int i = 0; i < s->nfns; i++) piv |= 1ull << s->pivot[i];
    s->col_mask = ((1ull << s->row_lo) - 1) & ~piv;
    for (int b = 0, j = 0; b < s->row_lo; b++) if ((s->col_mask >> b) & 1) s->col_pos[j++] = b;

    /* easy-mode candidates: the truth, a sibling from the same family, one from another family */
    {
        dsim_fnset_t set[3];
        set[0].n = set[1].n = set[2].n = s->nfns;
        memcpy(set[0].mask, s->fn, sizeof(uint64_t) * (size_t)s->nfns);
        int piv_tmp[MAX_FNS];
        for (int k = 1; k <= 2; k++) {
            int tries = 0;
            for (;;) {
                int tm = (k == 1) ? s->template_id : (s->template_id + 1 + (int)rng_below(&r, 2)) % 3;
                gen_mapping(&r, s->nfns, s->addr_bits, tm, set[k].mask, piv_tmp);
                int clash = span_equal_restricted(set[k].mask, s->nfns, set[0].mask, s->nfns);
                if (k == 2) clash |= span_equal_restricted(set[2].mask, s->nfns, set[1].mask, s->nfns);
                if (!clash || ++tries > 1000) break;
            }
        }
        int perm[3] = {0, 1, 2};
        for (int i = 2; i > 0; i--) { int j = (int)rng_below(&r, (unsigned)i + 1); int t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
        for (int i = 0; i < 3; i++) { s->cands[i] = set[perm[i]]; if (perm[i] == 0) s->cand_true = i; }
    }

    /* timing (DDR4-2400-ish, jittered per seed so thresholds cannot be hard-coded) */
    s->tCL = 12.5 + 2.5 * rng_unit(&r);
    s->tRCD = s->tCL;
    s->tRP = s->tCL;
    s->tRAS = 32.0;
    s->tRC = s->tRAS + s->tRP;
    s->tRFC = 350.0;
    s->tREFI = 7800.0;
    s->tREFW = s->tREFI * REFS_PER_WIN; /* shortened window (8 ms), see README */
    s->overhead = 40.0 + 15.0 * rng_unit(&r);
    s->cpu_ns = 6.0 + 4.0 * rng_unit(&r);
    s->ghz = floor((2.6 + 1.0 * rng_unit(&r)) * 10.0 + 0.5) / 10.0;
    s->sigma_cyc = 5.0 + 5.0 * rng_unit(&r);
    s->spike_p = 0.001 + 0.002 * rng_unit(&r);
    s->bg_p = 0.2 + 0.2 * rng_unit(&r);

    /* disturbance */
    s->w2 = 0.15 + 0.10 * rng_unit(&r);
    s->f_same = 0.4 + 0.2 * rng_unit(&r);
    s->hc_median = 18000.0 + 6000.0 * rng_unit(&r);
    s->hc_sigma = 0.8;
    s->orient_block = 256u << rng_below(&r, 3);

    /* TRR */
    s->trr_entries = 3 + (int)rng_below(&r, 4);
    s->trr_every = 2 << rng_below(&r, 2);
    s->trr_threshold = 8 << rng_below(&r, 2);

    /* the attacker's buffer: 1 GiB, physically contiguous, 1 GiB aligned */
    uint64_t mem = 1ull << s->addr_bits;
    s->info.mem_bytes = mem;
    s->info.buf_size = 1ull << BUF_BITS;
    s->info.buf_base = (uint64_t)rng_below(&r, (unsigned)(mem >> BUF_BITS)) << BUF_BITS;
    s->info.nbanks = 1u << s->nfns;
    s->info.rows_per_bank = ROWS_PER_BANK;
    s->info.row_bytes = ROW_BYTES;
    s->info.cpu_ghz = s->ghz;
    s->info.tREFI_ns = s->tREFI;
    s->info.tREFW_ns = s->tREFW;
    s->info.tRFC_ns = s->tRFC;
    s->info.tRC_ns = s->tRC;
    s->info.refs_per_window = REFS_PER_WIN;
    s->info.max_act_per_window = (uint64_t)(s->tREFW / s->tRC);
    s->info.mac = (uint64_t)MAC_FLOOR;
    s->info.blast_radius = 2;
    s->info.w2_max = W2_MAX;

    /* dynamic state */
    s->noise.s = mix64(o->seed ^ 0x6E6F697365ull);
    s->t_first_ref = rng_unit(&r) * s->tREFI;
    s->next_ref = s->t_first_ref;
    s->ref_busy_until = -1.0;
    size_t nb = s->info.nbanks, nrows = nb * ROWS_PER_BANK;
    s->open_row = (int32_t *)malloc(nb * sizeof(int32_t));
    s->last_act = (double *)malloc(nb * sizeof(double));
    s->rs = (rowstate_t **)calloc(nrows, sizeof(rowstate_t *));
    s->data = (uint64_t **)calloc(nrows, sizeof(uint64_t *));
    s->trr_row = (uint32_t *)calloc(nb * (size_t)s->trr_entries, sizeof(uint32_t));
    s->trr_cnt = (uint32_t *)calloc(nb * (size_t)s->trr_entries, sizeof(uint32_t));
    s->trr_valid = (uint8_t *)calloc(nb * (size_t)s->trr_entries, 1);
    s->trr_stamp = (uint64_t *)calloc(nb * (size_t)s->trr_entries, sizeof(uint64_t));
    s->trr_rng.s = mix64(o->seed ^ 0x545252ull);
    if (!s->open_row || !s->last_act || !s->rs || !s->data || !s->trr_row || !s->trr_cnt || !s->trr_valid ||
        !s->trr_stamp) {
        dsim_destroy(s);
        return NULL;
    }
    for (size_t b = 0; b < nb; b++) { s->open_row[b] = -1; s->last_act[b] = -1e9; }
    return s;
}

void dsim_destroy(dsim_t *s) {
    if (!s) return;
    size_t nrows = (size_t)s->info.nbanks * ROWS_PER_BANK;
    if (s->rs) for (size_t i = 0; i < nrows; i++) free(s->rs[i]);
    if (s->data) for (size_t i = 0; i < nrows; i++) free(s->data[i]);
    free(s->rs); free(s->data); free(s->open_row); free(s->last_act);
    free(s->trr_row); free(s->trr_cnt); free(s->trr_valid); free(s->trr_stamp);
    free(s);
}

/* ------------------------------------------------------------------ oracle (grader only) */
void dsim_oracle_truth(const dsim_t *s, dsim_truth_t *t) {
    memset(t, 0, sizeof *t);
    t->nfns = s->nfns;
    for (int i = 0; i < s->nfns; i++) { t->fn[i] = s->fn[i]; t->pivot[i] = s->pivot[i]; }
    t->row_lo = s->row_lo;
    t->addr_bits = s->addr_bits;
    t->buf_bits = BUF_BITS;
    t->col_mask = s->col_mask;
    t->template_id = s->template_id;
    t->template_name = template_names[s->template_id];
    t->cand_true = s->cand_true;
    t->trr_entries = s->trr_entries;
    t->trr_every = s->trr_every;
    t->trr_threshold = s->trr_threshold;
    t->w2 = s->w2;
    t->f_same = s->f_same;
    t->hc_median = s->hc_median;
    t->orient_block = s->orient_block;
}
void dsim_oracle_decode(const dsim_t *s, uint64_t pa, unsigned *bank, unsigned *row, unsigned *col) {
    decode(s, pa, bank, row, col);
}
uint64_t dsim_oracle_compose(const dsim_t *s, unsigned bank, unsigned row, unsigned col) {
    return compose(s, bank, row, col);
}
double dsim_oracle_row_threshold(const dsim_t *s, unsigned bank, unsigned row) {
    rowstate_t tmp;
    init_rowstate(s, bank, row, &tmp);
    double m = 1e30;
    for (int k = 0; k < tmp.n; k++) if (tmp.cell[k].thr < m) m = tmp.cell[k].thr;
    return m;
}
int dsim_oracle_row_orientation(const dsim_t *s, unsigned bank, unsigned row) {
    rowstate_t tmp;
    init_rowstate(s, bank, row, &tmp);
    return tmp.orient;
}
uint64_t dsim_oracle_restrict(const dsim_t *s, uint64_t mask) { (void)s; return restrict_mask(mask); }
