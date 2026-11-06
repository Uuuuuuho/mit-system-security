/* logbook.c - implementation of logbook.h.  REFERENCE SOLUTION (spoilers!).
 *
 * Every repaired line is marked FIX(Bn); the bug list is at the end of this file.
 */
#include "logbook.h"

#include <stdlib.h>
#include <string.h>

#define FNV_OFFSET 2166136261u
#define FNV_PRIME 16777619u

/* Bytes needed for a logbook with `cap` slots, or 0 if that does not fit in a size_t. */
static size_t logbook_size(size_t cap) {
    if (cap > (SIZE_MAX - sizeof(logbook)) / sizeof(lb_entry *)) return 0; /* FIX(B10) */
    return sizeof(logbook) + cap * sizeof(lb_entry *);
}

/* A new entry holding a copy of text, or NULL on allocation failure / absurd length. */
static lb_entry *entry_new(uint32_t seq, uint16_t tag, const char *text) {
    size_t len = strlen(text);
    if (len >= UINT32_MAX) return NULL; /* len + 1 must fit in the 32-bit cap field */
    lb_entry *e = malloc(sizeof *e + len + 1);
    if (!e) return NULL;
    e->seq = seq;
    e->tag = tag;
    e->reserved = 0;
    e->len = (uint32_t)len;
    e->cap = (uint32_t)(len + 1);
    e->flags = 0; /* FIX(B6) */
    memcpy(e->text, text, len + 1);
    return e;
}

/* Position of the entry with sequence number seq, or -1. */
static long find_index(const logbook *lb, uint32_t seq) {
    if (!lb) return -1;
    for (size_t i = 0; i < lb->count; i++)
        if (lb->slots[i]->seq == seq) return (long)i;
    return -1;
}

logbook *lb_create(size_t initial_cap) {
    if (initial_cap == 0) initial_cap = LB_DEFAULT_CAP;
    size_t bytes = logbook_size(initial_cap);
    if (bytes == 0) return NULL; /* FIX(B10) */
    logbook *lb = malloc(bytes);
    if (!lb) return NULL;
    lb->count = 0;
    lb->cap = initial_cap;
    lb->next_seq = 1;
    lb->labels_len = 0;
    lb->labels = NULL;
    return lb;
}

void lb_destroy(logbook *lb) {
    if (!lb) return;
    for (size_t i = 0; i < lb->count; i++) free(lb->slots[i]);
    free(lb->labels);
    free(lb);
}

int lb_reserve(logbook **lbp, size_t cap) {
    if (!lbp || !*lbp) return -1;
    if (cap <= (*lbp)->cap) return 0;
    size_t bytes = logbook_size(cap);
    if (bytes == 0) return -1;              /* FIX(B10) */
    logbook *grown = realloc(*lbp, bytes);  /* FIX(B7): keep the old block if realloc fails */
    if (!grown) return -1;
    grown->cap = cap;
    *lbp = grown;
    return 0;
}

int lb_append(logbook **lbp, uint16_t tag, const char *text, uint32_t *seq_out) {
    if (!lbp || !*lbp || !text) return -1;
    logbook *lb = *lbp;
    lb_entry *e = entry_new(lb->next_seq, tag, text);
    if (!e) return -1;
    if (lb->count == lb->cap) {
        if (lb->cap > SIZE_MAX / 2 || lb_reserve(lbp, lb->cap * 2) != 0) {
            free(e);
            return -1;
        }
        lb = *lbp; /* FIX(B3): the block may have moved */
    }
    lb->slots[lb->count++] = e;
    if (seq_out) *seq_out = e->seq;
    lb->next_seq++;
    return 0;
}

size_t lb_count(const logbook *lb) { return lb ? lb->count : 0; }

const lb_entry *lb_get(const logbook *lb, size_t idx) {
    if (!lb || idx >= lb->count) return NULL;
    return lb->slots[idx];
}

const lb_entry *lb_find(const logbook *lb, uint32_t seq) {
    long i = find_index(lb, seq);
    return i < 0 ? NULL : lb->slots[i];
}

int lb_edit(logbook *lb, uint32_t seq, const char *text) {
    if (!text) return -1;
    long i = find_index(lb, seq);
    if (i < 0) return -1;
    size_t len = strlen(text);
    if (len >= UINT32_MAX) return -1;
    lb_entry *e = lb->slots[i];
    if (len + 1 > e->cap) {
        lb_entry *bigger = realloc(e, sizeof *e + len + 1); /* FIX(B1): room for the NUL */
        if (!bigger) return -1;
        bigger->cap = (uint32_t)(len + 1);
        lb->slots[i] = e = bigger;
    }
    memmove(e->text, text, len + 1);
    e->len = (uint32_t)len;
    return 0;
}

int lb_remove(logbook *lb, uint32_t seq) {
    long i = find_index(lb, seq);
    if (i < 0) return -1;
    free(lb->slots[i]); /* FIX(B5) */
    memmove(&lb->slots[i], &lb->slots[i + 1], (lb->count - (size_t)i - 1) * sizeof lb->slots[0]);
    lb->count--;
    return 0;
}

int lb_mark(logbook *lb, uint32_t seq, uint32_t flags) {
    long i = find_index(lb, seq);
    if (i < 0) return -1;
    lb->slots[i]->flags |= flags;
    return 0;
}

size_t lb_count_flagged(const logbook *lb, uint32_t mask) {
    size_t n = 0;
    for (size_t i = 0; lb && i < lb->count; i++)
        if (lb->slots[i]->flags & mask) n++;
    return n;
}

uint32_t *lb_seqs(const logbook *lb) {
    size_t n = lb ? lb->count : 0;
    uint32_t *v = malloc((n + 1) * sizeof *v); /* FIX(B2): count + 1 elements */
    if (!v) return NULL;
    for (size_t i = 0; i < n; i++) v[i] = lb->slots[i]->seq;
    v[n] = 0;
    return v;
}

int lb_add_label(logbook *lb, const char *label) {
    if (!lb || !label || !label[0] || strchr(label, ',')) return -1;
    size_t len = strlen(label);
    size_t sep = lb->labels_len ? 1 : 0;
    /* FIX(B8): add up what we need instead of subtracting from the limit. The old check
     * `len > LB_LABELS_MAX - 2 - labels_len` wrapped around once labels_len == LB_LABELS_MAX - 1. */
    if (len >= LB_LABELS_MAX || lb->labels_len + sep + len + 1 > LB_LABELS_MAX) return -1;
    if (!lb->labels) {
        lb->labels = malloc(LB_LABELS_MAX);
        if (!lb->labels) return -1;
    }
    if (sep) lb->labels[lb->labels_len++] = ',';
    memcpy(lb->labels + lb->labels_len, label, len + 1);
    lb->labels_len += len;
    return 0;
}

const char *lb_labels(const logbook *lb) {
    return (lb && lb->labels) ? lb->labels : "";
}

uint32_t lb_checksum(const logbook *lb) {
    uint32_t h = FNV_OFFSET; /* FIX(B9): unsigned arithmetic wraps; signed overflow is UB */
    for (size_t i = 0; lb && i < lb->count; i++) {
        const lb_entry *e = lb->slots[i];
        for (uint32_t k = 0; k < e->len; k++) h = (h ^ (unsigned char)e->text[k]) * FNV_PRIME;
        h = (h ^ (unsigned char)'\n') * FNV_PRIME;
    }
    return h;
}

void lb_clear(logbook *lb) {
    if (!lb) return;
    for (size_t i = 0; i < lb->count; i++) free(lb->slots[i]);
    lb->count = 0;
    free(lb->labels);
    lb->labels = NULL; /* FIX(B4): otherwise lb_destroy() frees it a second time */
    lb->labels_len = 0;
}

/* ---------------------------------------------------------------------------------------------
 * Instructor notes: the ten planted bugs in src/logbook.c
 *
 *  B1  lb_edit       off-by-one: realloc(e, sizeof *e + len) has no room for the NUL, then
 *                    len + 1 bytes are copied -> 1-byte heap overflow.          CHECK_3 (ASan, valgrind)
 *  B2  lb_seqs       heap out-of-bounds write: allocates count elements, writes count + 1
 *                    (the 0 terminator).                                        CHECK_4 (ASan, valgrind)
 *  B3  lb_append     use after free: `lb` is not reloaded after lb_reserve() moved the block,
 *                    so the new slot and count++ go to the freed old block.     CHECK_2 (all modes)
 *  B4  lb_clear      dangling pointer -> double free: labels freed but not set to NULL; the
 *                    next lb_labels/lb_add_label use it, lb_destroy frees again. CHECK_5 (all modes)
 *  B5  lb_remove     memory leak: the removed entry is never freed.             CHECK_6 (shim, LSan, valgrind)
 *  B6  entry_new     uninitialized read: flags not set to 0, so lb_mark ORs into garbage and
 *                    lb_count_flagged counts garbage bits.                     CHECK_7 (shim fill, ASan fill, valgrind)
 *  B7  lb_reserve    wrong NULL handling: `*lbp = realloc(*lbp, ...)` loses (leaks) the
 *                    logbook and returns with *lbp == NULL when realloc fails. CHECK_11 (fault injection)
 *  B8  lb_add_label  CVE-2022-0185-style length underflow: LB_LABELS_MAX - 2 - labels_len wraps
 *                    to SIZE_MAX once labels_len == LB_LABELS_MAX - 1, so any label passes the
 *                    check and is copied past the 32-byte buffer. (Also wrongly rejects a
 *                    31-character first label.)                                CHECK_8 (all modes)
 *  B9  lb_checksum   signed integer overflow (UB): FNV-1a computed in `int`. Gives the right
 *                    answer at -O2 on gcc/clang today, which is exactly the problem.  CHECK_9 (UBSan)
 *  B10 lb_create /   integer overflow in a size computation: sizeof(logbook) + cap * 8 wraps for
 *      lb_reserve    huge cap, a tiny block is allocated and reported as success.   CHECK_10 (all modes)
 */
