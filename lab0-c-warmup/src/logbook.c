/* logbook.c - implementation of logbook.h.
 *
 * TODO(Exercise 0-1): this implementation compiles cleanly and passes a casual smoke test, but
 * it contains TEN planted bugs (memory-safety bugs, undefined behaviour and spec violations).
 * Find and fix them all without changing logbook.h. `make check` tells you which behaviours
 * still fail; ASan/UBSan (./tester-asan), valgrind and gdb tell you where.
 * Every function below is marked "audit me"; some of them are actually correct.
 */
#include "logbook.h"

#include <stdlib.h>
#include <string.h>

#define FNV_OFFSET 2166136261u
#define FNV_PRIME 16777619

/* audit me: a new entry holding a copy of text, or NULL on allocation failure. */
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
    memcpy(e->text, text, len + 1);
    return e;
}

/* audit me: position of the entry with sequence number seq, or -1. */
static long find_index(const logbook *lb, uint32_t seq) {
    if (!lb) return -1;
    for (size_t i = 0; i < lb->count; i++)
        if (lb->slots[i]->seq == seq) return (long)i;
    return -1;
}

/* audit me */
logbook *lb_create(size_t initial_cap) {
    if (initial_cap == 0) initial_cap = LB_DEFAULT_CAP;
    logbook *lb = malloc(sizeof(logbook) + initial_cap * sizeof(lb_entry *));
    if (!lb) return NULL;
    lb->count = 0;
    lb->cap = initial_cap;
    lb->next_seq = 1;
    lb->labels_len = 0;
    lb->labels = NULL;
    return lb;
}

/* audit me */
void lb_destroy(logbook *lb) {
    if (!lb) return;
    for (size_t i = 0; i < lb->count; i++) free(lb->slots[i]);
    free(lb->labels);
    free(lb);
}

/* audit me */
int lb_reserve(logbook **lbp, size_t cap) {
    if (!lbp || !*lbp) return -1;
    if (cap <= (*lbp)->cap) return 0;
    *lbp = realloc(*lbp, sizeof(logbook) + cap * sizeof(lb_entry *));
    if (!*lbp) return -1;
    (*lbp)->cap = cap;
    return 0;
}

/* audit me */
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
    }
    lb->slots[lb->count++] = e;
    if (seq_out) *seq_out = e->seq;
    lb->next_seq++;
    return 0;
}

/* audit me */
size_t lb_count(const logbook *lb) { return lb ? lb->count : 0; }

/* audit me */
const lb_entry *lb_get(const logbook *lb, size_t idx) {
    if (!lb || idx >= lb->count) return NULL;
    return lb->slots[idx];
}

/* audit me */
const lb_entry *lb_find(const logbook *lb, uint32_t seq) {
    long i = find_index(lb, seq);
    return i < 0 ? NULL : lb->slots[i];
}

/* audit me */
int lb_edit(logbook *lb, uint32_t seq, const char *text) {
    if (!text) return -1;
    long i = find_index(lb, seq);
    if (i < 0) return -1;
    size_t len = strlen(text);
    if (len >= UINT32_MAX) return -1;
    lb_entry *e = lb->slots[i];
    if (len + 1 > e->cap) {
        lb_entry *bigger = realloc(e, sizeof *e + len);
        if (!bigger) return -1;
        bigger->cap = (uint32_t)(len + 1);
        lb->slots[i] = e = bigger;
    }
    memmove(e->text, text, len + 1);
    e->len = (uint32_t)len;
    return 0;
}

/* audit me */
int lb_remove(logbook *lb, uint32_t seq) {
    long i = find_index(lb, seq);
    if (i < 0) return -1;
    memmove(&lb->slots[i], &lb->slots[i + 1], (lb->count - (size_t)i - 1) * sizeof lb->slots[0]);
    lb->count--;
    return 0;
}

/* audit me */
int lb_mark(logbook *lb, uint32_t seq, uint32_t flags) {
    long i = find_index(lb, seq);
    if (i < 0) return -1;
    lb->slots[i]->flags |= flags;
    return 0;
}

/* audit me */
size_t lb_count_flagged(const logbook *lb, uint32_t mask) {
    size_t n = 0;
    for (size_t i = 0; lb && i < lb->count; i++)
        if (lb->slots[i]->flags & mask) n++;
    return n;
}

/* audit me */
uint32_t *lb_seqs(const logbook *lb) {
    size_t n = lb ? lb->count : 0;
    uint32_t *v = malloc(n * sizeof *v);
    if (!v) return NULL;
    for (size_t i = 0; i < n; i++) v[i] = lb->slots[i]->seq;
    v[n] = 0;
    return v;
}

/* audit me */
int lb_add_label(logbook *lb, const char *label) {
    if (!lb || !label || !label[0] || strchr(label, ',')) return -1;
    size_t len = strlen(label);
    /* need room for the ',' separator and the terminating NUL */
    if (len > LB_LABELS_MAX - 2 - lb->labels_len) return -1;
    if (!lb->labels) {
        lb->labels = malloc(LB_LABELS_MAX);
        if (!lb->labels) return -1;
    }
    if (lb->labels_len) lb->labels[lb->labels_len++] = ',';
    memcpy(lb->labels + lb->labels_len, label, len + 1);
    lb->labels_len += len;
    return 0;
}

/* audit me */
const char *lb_labels(const logbook *lb) {
    return (lb && lb->labels) ? lb->labels : "";
}

/* audit me */
uint32_t lb_checksum(const logbook *lb) {
    int h = (int)FNV_OFFSET;
    for (size_t i = 0; lb && i < lb->count; i++) {
        const lb_entry *e = lb->slots[i];
        for (uint32_t k = 0; k < e->len; k++) h = (h ^ (unsigned char)e->text[k]) * FNV_PRIME;
        h = (h ^ '\n') * FNV_PRIME;
    }
    return (uint32_t)h;
}

/* audit me */
void lb_clear(logbook *lb) {
    if (!lb) return;
    for (size_t i = 0; i < lb->count; i++) free(lb->slots[i]);
    lb->count = 0;
    free(lb->labels);
    lb->labels_len = 0;
}
