/* logbook.h - a tiny append-only journal of tagged text entries.
 *
 * This header is the SPECIFICATION. The implementation in logbook.c is supposed to follow it
 * exactly; where the two disagree, the header wins.
 *
 * Memory layout
 * -------------
 *   logbook  : one heap block = fixed header + flexible array member `slots[cap]` of entry
 *              pointers. Growing the logbook reallocs the WHOLE block, so the block may move.
 *              That is why every function that can grow it takes `logbook **lbp` and updates
 *              *lbp (callers must re-read *lbp afterwards).
 *   lb_entry : one heap block per entry = fixed header + flexible array member `text[len + 1]`
 *              (NUL-terminated copy of the caller's string).
 *   labels   : an optional heap buffer of exactly LB_LABELS_MAX bytes holding a comma-separated,
 *              NUL-terminated list of labels, allocated on the first lb_add_label().
 *
 * Conventions
 * -----------
 *   - Functions returning int return 0 on success and -1 on any error (bad argument, quota,
 *     out of memory, arithmetic overflow). On error, the logbook is left UNCHANGED and valid.
 *   - Entries are kept in append order. Indices (lb_get) are 0-based positions in that order;
 *     sequence numbers (seq) are 1, 2, 3, ... assigned at append time and never reused, even
 *     after lb_remove() or lb_clear().
 *   - Pointers returned by lb_get()/lb_find() stay valid until the next call that modifies the
 *     logbook (append, edit, remove, clear, reserve, destroy).
 *   - Every allocation is released by lb_destroy() (or earlier); nothing leaks.
 */
#ifndef LOGBOOK_H
#define LOGBOOK_H

#include <stddef.h>
#include <stdint.h>

#define LB_DEFAULT_CAP 8   /* capacity used when lb_create(0) is called */
#define LB_LABELS_MAX 32   /* size of the labels buffer, INCLUDING the terminating NUL */

/* entry flag bits for lb_mark() / lb_count_flagged() */
#define LB_F_STARRED 0x1u
#define LB_F_ARCHIVED 0x2u
#define LB_F_PINNED 0x4u

typedef struct lb_entry {
    uint32_t seq;      /* sequence number (>= 1) */
    uint16_t tag;      /* caller-chosen tag */
    uint16_t reserved; /* always 0 */
    uint32_t len;      /* strlen(text) */
    uint32_t cap;      /* bytes available in text[] (>= len + 1) */
    uint32_t flags;    /* LB_F_* bits; 0 when the entry is created */
    char text[];       /* flexible array member: NUL-terminated text */
} lb_entry;

typedef struct logbook {
    size_t count;        /* number of entries in slots[0 .. count-1] */
    size_t cap;          /* number of slots allocated */
    uint32_t next_seq;   /* seq that the next appended entry gets */
    size_t labels_len;   /* strlen(labels), 0 if no labels */
    char *labels;        /* NULL or a heap buffer of LB_LABELS_MAX bytes */
    lb_entry *slots[];   /* flexible array member: cap entry pointers */
} logbook;

/* Create an empty logbook with room for `initial_cap` entries (LB_DEFAULT_CAP if 0).
 * Returns NULL if memory cannot be allocated or the size computation would overflow. */
logbook *lb_create(size_t initial_cap);

/* Free the logbook, all its entries and its labels. lb_destroy(NULL) does nothing. */
void lb_destroy(logbook *lb);

/* Make sure there is room for at least `cap` entries (never shrinks). May move the logbook:
 * on success *lbp is updated. Returns -1 (and leaves *lbp and its contents untouched) if the
 * required size cannot be represented in a size_t or the allocation fails. */
int lb_reserve(logbook **lbp, size_t cap);

/* Append a copy of `text` with tag `tag`. Grows the logbook (doubling its capacity) when it is
 * full, which may move it (*lbp is updated). If seq_out is not NULL, the new entry's sequence
 * number is stored there. Returns -1 if lbp, *lbp or text is NULL, or on allocation failure. */
int lb_append(logbook **lbp, uint16_t tag, const char *text, uint32_t *seq_out);

/* Number of entries (0 for NULL). */
size_t lb_count(const logbook *lb);

/* Entry at 0-based position idx, or NULL if idx >= count (or lb is NULL). */
const lb_entry *lb_get(const logbook *lb, size_t idx);

/* Entry with sequence number seq, or NULL if there is none. */
const lb_entry *lb_find(const logbook *lb, uint32_t seq);

/* Replace the text of entry `seq` by a copy of `text` (any length). seq, tag and flags are kept.
 * Returns -1 if there is no such entry, text is NULL, or memory runs out. */
int lb_edit(logbook *lb, uint32_t seq, const char *text);

/* Remove entry `seq`; later entries move down one position. Returns -1 if there is no such entry. */
int lb_remove(logbook *lb, uint32_t seq);

/* OR `flags` into the flags of entry `seq`. Returns -1 if there is no such entry. */
int lb_mark(logbook *lb, uint32_t seq, uint32_t flags);

/* Number of entries whose flags have at least one bit of `mask` set. */
size_t lb_count_flagged(const logbook *lb, uint32_t mask);

/* A new heap array with the sequence numbers of all entries in order, followed by a 0
 * terminator (so count + 1 elements). The caller frees it. NULL on allocation failure. */
uint32_t *lb_seqs(const logbook *lb);

/* Append `label` to the labels list: "a" -> "a,b" -> "a,b,c". The label must be non-empty and
 * must not contain ','. Returns -1 (labels unchanged) if it is invalid or if the resulting
 * string, including its NUL terminator, would not fit in LB_LABELS_MAX bytes. */
int lb_add_label(logbook *lb, const char *label);

/* The labels string ("" if there are none). Valid until the next modifying call. */
const char *lb_labels(const logbook *lb);

/* 32-bit FNV-1a hash over all entries in order: for each entry hash its text bytes followed by
 * one '\n' byte. Offset basis 2166136261, prime 16777619, arithmetic modulo 2^32.
 * An empty logbook hashes to 2166136261. */
uint32_t lb_checksum(const logbook *lb);

/* Remove all entries and all labels. The capacity is kept and seq numbering continues. The
 * logbook stays fully usable (append, add_label, ...) afterwards. */
void lb_clear(logbook *lb);

#endif
