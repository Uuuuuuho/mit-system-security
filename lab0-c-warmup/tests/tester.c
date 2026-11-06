/* tester.c - numbered behavioural checks for the logbook library.
 *
 *   ./tester              run every check, each in its own child process, and print a summary
 *   ./tester CHECK_3      run one check in this process (use this under gdb / valgrind)
 *   ./tester --list       list the checks
 *
 * Each check exercises one area of logbook.h and ends with a leak check: every block allocated
 * during the check must be freed again (see alloc_shim.h). Build flavours:
 *   ./tester        -O2, plain libc malloc wrapped by the shim
 *   ./tester-asan   -O1, AddressSanitizer + UndefinedBehaviorSanitizer, aborts on the first error
 * `make check` runs every check in both flavours and under valgrind (if installed); a check
 * passes only if it passes everywhere.
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "alloc_shim.h"
#include "logbook.h"

static int failures;
static size_t baseline;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            printf("    check failed (%s:%d): %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                            \
        }                                                                          \
    } while (0)
/* like CHECK, but stop the check: continuing would dereference something bogus */
#define REQUIRE(cond)                                                              \
    do {                                                                           \
        if (!(cond)) {                                                             \
            printf("    requirement failed (%s:%d): %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                            \
            return;                                                                \
        }                                                                          \
    } while (0)

static void begin(void) { baseline = shim_live_blocks(); }
static void expect_no_leaks(void) {
    size_t now = shim_live_blocks();
    if (now != baseline) {
        printf("    leak: %zu block(s) still allocated after the check\n", now - baseline);
        shim_dump_live(5);
        failures++;
    }
}

static uint32_t fnv1a_ref(const char *const *texts, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        for (const unsigned char *p = (const unsigned char *)texts[i]; *p; p++) h = (h ^ *p) * 16777619u;
        h = (h ^ (unsigned char)'\n') * 16777619u;
    }
    return h;
}

/* CHECK_1: create / append / get / find on a small logbook (sanity) */
static void check_basics(void) {
    begin();
    logbook *lb = lb_create(0);
    REQUIRE(lb != NULL);
    CHECK(lb->cap == LB_DEFAULT_CAP);
    CHECK(lb_count(lb) == 0);
    CHECK(strcmp(lb_labels(lb), "") == 0);
    const char *texts[] = {"alpha", "", "gamma ray burst"};
    for (int i = 0; i < 3; i++) {
        uint32_t seq = 0;
        CHECK(lb_append(&lb, (uint16_t)(10 + i), texts[i], &seq) == 0);
        CHECK(seq == (uint32_t)(i + 1));
    }
    CHECK(lb_append(&lb, 1, NULL, NULL) == -1);
    CHECK(lb_count(lb) == 3);
    for (int i = 0; i < 3; i++) {
        const lb_entry *e = lb_get(lb, (size_t)i);
        REQUIRE(e != NULL);
        CHECK(e->seq == (uint32_t)(i + 1));
        CHECK(e->tag == 10 + i);
        CHECK(e->reserved == 0);
        CHECK(e->len == strlen(texts[i]));
        CHECK(e->cap >= e->len + 1);
        CHECK(strcmp(e->text, texts[i]) == 0);
        CHECK(lb_find(lb, e->seq) == e);
    }
    CHECK(lb_get(lb, 3) == NULL);
    CHECK(lb_find(lb, 0) == NULL);
    CHECK(lb_find(lb, 4) == NULL);
    CHECK(lb_count(NULL) == 0 && lb_get(NULL, 0) == NULL);
    lb_destroy(lb);
    lb_destroy(NULL);
    expect_no_leaks();
}

/* CHECK_2: many appends starting from a tiny capacity (the logbook must grow and may move) */
static void check_growth(void) {
    begin();
    logbook *lb = lb_create(2);
    REQUIRE(lb != NULL);
    char buf[32];
    for (int i = 0; i < 100; i++) {
        snprintf(buf, sizeof buf, "entry %d", i);
        REQUIRE(lb_append(&lb, (uint16_t)i, buf, NULL) == 0);
        REQUIRE(lb != NULL);
    }
    CHECK(lb_count(lb) == 100);
    CHECK(lb->cap >= 100);
    for (int i = 0; i < 100 && i < (int)lb_count(lb); i++) {
        const lb_entry *e = lb_get(lb, (size_t)i);
        REQUIRE(e != NULL);
        snprintf(buf, sizeof buf, "entry %d", i);
        CHECK(e->seq == (uint32_t)(i + 1));
        CHECK(strcmp(e->text, buf) == 0);
    }
    lb_destroy(lb);
    expect_no_leaks();
}

/* CHECK_3: lb_edit to longer and shorter texts */
static void check_edit(void) {
    begin();
    logbook *lb = lb_create(4);
    REQUIRE(lb != NULL);
    REQUIRE(lb_append(&lb, 7, "short", NULL) == 0);
    REQUIRE(lb_append(&lb, 8, "medium text", NULL) == 0);
    CHECK(lb_mark(lb, 1, LB_F_STARRED) == 0);
    const char *longer = "a considerably longer replacement text";
    CHECK(lb_edit(lb, 1, longer) == 0);
    const lb_entry *e = lb_find(lb, 1);
    REQUIRE(e != NULL);
    CHECK(strcmp(e->text, longer) == 0);
    CHECK(e->len == strlen(longer) && e->cap >= e->len + 1);
    CHECK(e->seq == 1 && e->tag == 7);
    CHECK((e->flags & LB_F_STARRED) != 0);
    CHECK(lb_edit(lb, 1, "tiny") == 0);
    e = lb_find(lb, 1);
    REQUIRE(e != NULL);
    CHECK(strcmp(e->text, "tiny") == 0 && e->len == 4);
    CHECK(lb_edit(lb, 2, "medium text, now with a much longer tail") == 0);
    CHECK(lb_edit(lb, 2, "") == 0);
    e = lb_get(lb, 1);
    REQUIRE(e != NULL);
    CHECK(e->seq == 2 && e->len == 0 && e->text[0] == '\0');
    CHECK(lb_edit(lb, 99, "nope") == -1);
    CHECK(lb_edit(lb, 1, NULL) == -1);
    CHECK(lb_count(lb) == 2);
    lb_destroy(lb);
    expect_no_leaks();
}

/* CHECK_4: lb_seqs returns count + 1 elements with a 0 terminator */
static void check_seqs(void) {
    begin();
    logbook *lb = lb_create(0);
    REQUIRE(lb != NULL);
    uint32_t *v = lb_seqs(lb);
    REQUIRE(v != NULL);
    CHECK(v[0] == 0);
    free(v);
    for (int i = 0; i < 5; i++) REQUIRE(lb_append(&lb, 0, "x", NULL) == 0);
    v = lb_seqs(lb);
    REQUIRE(v != NULL);
    for (int i = 0; i < 5; i++) CHECK(v[i] == (uint32_t)(i + 1));
    CHECK(v[5] == 0);
    free(v);
    lb_destroy(lb);
    expect_no_leaks();
}

/* CHECK_5: lb_clear, then keep using the logbook */
static void check_clear(void) {
    begin();
    logbook *lb = lb_create(0);
    REQUIRE(lb != NULL);
    for (int i = 0; i < 3; i++) REQUIRE(lb_append(&lb, 0, "before", NULL) == 0);
    CHECK(lb_add_label(lb, "x") == 0);
    CHECK(lb_add_label(lb, "y") == 0);
    CHECK(strcmp(lb_labels(lb), "x,y") == 0);
    lb_clear(lb);
    CHECK(lb_count(lb) == 0);
    CHECK(strcmp(lb_labels(lb), "") == 0);
    uint32_t seq = 0;
    CHECK(lb_append(&lb, 0, "after", &seq) == 0);
    CHECK(seq == 4); /* numbering continues */
    REQUIRE(lb_get(lb, 0) != NULL);
    CHECK(lb_get(lb, 0)->seq == 4);
    CHECK(lb_add_label(lb, "z") == 0);
    CHECK(strcmp(lb_labels(lb), "z") == 0);
    lb_destroy(lb);
    expect_no_leaks();
}

/* CHECK_6: lb_remove at the front, middle and end */
static void check_remove(void) {
    begin();
    logbook *lb = lb_create(0);
    REQUIRE(lb != NULL);
    char buf[16];
    for (int i = 1; i <= 5; i++) {
        snprintf(buf, sizeof buf, "e%d", i);
        REQUIRE(lb_append(&lb, 0, buf, NULL) == 0);
    }
    CHECK(lb_remove(lb, 3) == 0);
    CHECK(lb_count(lb) == 4);
    CHECK(lb_find(lb, 3) == NULL);
    CHECK(lb_remove(lb, 1) == 0);
    CHECK(lb_remove(lb, 5) == 0);
    CHECK(lb_remove(lb, 3) == -1);
    REQUIRE(lb_count(lb) == 2);
    CHECK(lb_get(lb, 0)->seq == 2 && strcmp(lb_get(lb, 0)->text, "e2") == 0);
    CHECK(lb_get(lb, 1)->seq == 4 && strcmp(lb_get(lb, 1)->text, "e4") == 0);
    uint32_t seq = 0;
    CHECK(lb_append(&lb, 0, "e6", &seq) == 0 && seq == 6);
    CHECK(lb_count(lb) == 3 && lb_get(lb, 2) == lb_find(lb, 6));
    lb_destroy(lb);
    expect_no_leaks();
}

/* CHECK_7: flags start at 0; lb_mark / lb_count_flagged */
static void check_flags(void) {
    begin();
    logbook *lb = lb_create(0);
    REQUIRE(lb != NULL);
    for (int i = 0; i < 6; i++) REQUIRE(lb_append(&lb, 0, "flag me maybe", NULL) == 0);
    for (int i = 0; i < 6; i++) CHECK(lb_get(lb, (size_t)i)->flags == 0);
    CHECK(lb_mark(lb, 2, LB_F_STARRED) == 0);
    CHECK(lb_mark(lb, 5, LB_F_STARRED | LB_F_PINNED) == 0);
    CHECK(lb_mark(lb, 6, LB_F_ARCHIVED) == 0);
    CHECK(lb_mark(lb, 99, LB_F_ARCHIVED) == -1);
    CHECK(lb_count_flagged(lb, LB_F_STARRED) == 2);
    CHECK(lb_count_flagged(lb, LB_F_PINNED) == 1);
    CHECK(lb_count_flagged(lb, LB_F_ARCHIVED | LB_F_PINNED) == 2);
    CHECK(lb_count_flagged(lb, 0) == 0);
    CHECK(lb_find(lb, 1)->flags == 0);
    CHECK(lb_find(lb, 5)->flags == (LB_F_STARRED | LB_F_PINNED));
    lb_destroy(lb);
    expect_no_leaks();
}

/* CHECK_8: labels up to exactly LB_LABELS_MAX bytes, and not one byte more */
static void check_labels(void) {
    begin();
    logbook *lb = lb_create(0);
    REQUIRE(lb != NULL);
    CHECK(lb_add_label(lb, "") == -1);
    CHECK(lb_add_label(lb, "a,b") == -1);
    CHECK(lb_add_label(lb, NULL) == -1);
    CHECK(lb_add_label(lb, "abcdefghij") == 0);
    CHECK(lb_add_label(lb, "klmnopqrst") == 0);
    CHECK(lb_add_label(lb, "uvwxyzABC") == 0); /* 31 characters + NUL = 32 bytes: full */
    CHECK(strcmp(lb_labels(lb), "abcdefghij,klmnopqrst,uvwxyzABC") == 0);
    CHECK(lb_add_label(lb, "Z") == -1);        /* would need 34 bytes */
    CHECK(lb_add_label(lb, "toolong") == -1);
    CHECK(strcmp(lb_labels(lb), "abcdefghij,klmnopqrst,uvwxyzABC") == 0);
    lb_destroy(lb);

    lb = lb_create(0);
    REQUIRE(lb != NULL);
    const char *l31 = "0123456789012345678901234567890"; /* 31 characters */
    const char *l32 = "01234567890123456789012345678901";
    CHECK(lb_add_label(lb, l32) == -1);
    CHECK(lb_add_label(lb, l31) == 0);
    CHECK(lb_add_label(lb, "q") == -1);
    CHECK(strcmp(lb_labels(lb), l31) == 0);
    lb_destroy(lb);
    expect_no_leaks();
}

/* CHECK_9: lb_checksum matches FNV-1a */
static void check_checksum(void) {
    begin();
    logbook *lb = lb_create(0);
    REQUIRE(lb != NULL);
    CHECK(lb_checksum(lb) == 2166136261u);
    const char *texts[] = {"hello", "world", "", "\xff\xfe high bytes \x80", "the quick brown fox jumps over the lazy dog"};
    for (int i = 0; i < 5; i++) REQUIRE(lb_append(&lb, 0, texts[i], NULL) == 0);
    uint32_t want = fnv1a_ref(texts, 5), got = lb_checksum(lb);
    if (got != want) printf("    checksum %08" PRIx32 ", expected %08" PRIx32 "\n", got, want);
    CHECK(got == want);
    lb_destroy(lb);
    expect_no_leaks();
}

/* CHECK_10: absurd capacities are rejected instead of "succeeding" */
static void check_huge(void) {
    begin();
    logbook *bad = lb_create(SIZE_MAX / 4);
    CHECK(bad == NULL);
    lb_destroy(bad);
    bad = lb_create(SIZE_MAX / sizeof(lb_entry *) + 2);
    CHECK(bad == NULL);
    lb_destroy(bad);
    logbook *lb = lb_create(16);
    REQUIRE(lb != NULL);
    REQUIRE(lb_append(&lb, 0, "one", NULL) == 0 && lb_append(&lb, 0, "two", NULL) == 0);
    logbook *before = lb;
    CHECK(lb_reserve(&lb, SIZE_MAX / sizeof(lb_entry *) + 2) == -1);
    CHECK(lb_reserve(&lb, SIZE_MAX) == -1);
    REQUIRE(lb == before);
    CHECK(lb->cap == 16 && lb_count(lb) == 2);
    for (int i = 0; i < 10; i++) CHECK(lb_append(&lb, 0, "more", NULL) == 0); /* no growth needed */
    REQUIRE(lb != NULL);
    CHECK(lb_count(lb) == 12 && strcmp(lb_get(lb, 1)->text, "two") == 0);
    lb_destroy(lb);
    expect_no_leaks();
}

/* CHECK_11: out-of-memory paths leave everything intact (allocation failures are injected) */
static void check_oom(void) {
    begin();
    shim_fail_after(0);
    CHECK(lb_create(0) == NULL);
    logbook *lb = lb_create(4);
    REQUIRE(lb != NULL);
    for (int i = 0; i < 3; i++) REQUIRE(lb_append(&lb, 0, "keep me", NULL) == 0);
    logbook *before = lb;
    shim_fail_after(0);
    CHECK(lb_reserve(&lb, 64) == -1);
    REQUIRE(lb == before);
    CHECK(lb_count(lb) == 3 && lb->cap == 4);
    CHECK(strcmp(lb_get(lb, 2)->text, "keep me") == 0);
    shim_fail_after(0);
    CHECK(lb_append(&lb, 0, "no memory", NULL) == -1);
    REQUIRE(lb != NULL);
    CHECK(lb_count(lb) == 3);
    REQUIRE(lb_append(&lb, 0, "fills the last slot", NULL) == 0);
    shim_fail_after(1); /* the entry allocation succeeds, growing the logbook fails */
    CHECK(lb_append(&lb, 0, "needs growth", NULL) == -1);
    REQUIRE(lb != NULL);
    CHECK(lb_count(lb) == 4);
    shim_fail_after(-1);
    CHECK(lb_reserve(&lb, 64) == 0);
    REQUIRE(lb != NULL);
    CHECK(lb->cap >= 64 && lb_count(lb) == 4);
    CHECK(lb_append(&lb, 0, "fine again", NULL) == 0 && lb_count(lb) == 5);
    lb_destroy(lb);
    expect_no_leaks();
}

static const struct {
    const char *name;
    void (*fn)(void);
    const char *what;
} checks[] = {
    {"CHECK_1", check_basics, "create / append / get / find (sanity)"},
    {"CHECK_2", check_growth, "100 appends from capacity 2 (growth)"},
    {"CHECK_3", check_edit, "lb_edit to longer / shorter text"},
    {"CHECK_4", check_seqs, "lb_seqs array and terminator"},
    {"CHECK_5", check_clear, "lb_clear, then reuse"},
    {"CHECK_6", check_remove, "lb_remove front / middle / end"},
    {"CHECK_7", check_flags, "flags start at 0; mark / count_flagged"},
    {"CHECK_8", check_labels, "labels fill exactly LB_LABELS_MAX bytes"},
    {"CHECK_9", check_checksum, "FNV-1a checksum"},
    {"CHECK_10", check_huge, "absurd capacities are rejected"},
    {"CHECK_11", check_oom, "out-of-memory paths keep the logbook intact"},
};
#define NCHECKS (sizeof checks / sizeof checks[0])

static int run_one(size_t i) {
    failures = 0;
    printf("%s: %s\n", checks[i].name, checks[i].what);
    fflush(stdout);
    checks[i].fn();
    printf("%s: %s\n", checks[i].name, failures ? "FAIL" : "PASS");
    fflush(stdout);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < NCHECKS; i++) printf("%-9s %s\n", checks[i].name, checks[i].what);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--probe") == 0) { /* used by check.py: does this build run at all? */
        free(malloc(16));
        printf("probe ok\n");
        return 0;
    }
    if (argc == 2) {
        for (size_t i = 0; i < NCHECKS; i++)
            if (strcmp(argv[1], checks[i].name) == 0) return run_one(i);
        fprintf(stderr, "unknown check %s (try --list)\n", argv[1]);
        return 2;
    }
    /* all checks, each in a child process so that a crash only fails that check */
    int passed = 0;
    for (size_t i = 0; i < NCHECKS; i++) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) _exit(run_one(i));
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) passed++;
        else if (WIFSIGNALED(st)) printf("%s: FAIL (crashed with signal %d)\n", checks[i].name, WTERMSIG(st));
        else if (WIFEXITED(st) && WEXITSTATUS(st) != 1) printf("%s: FAIL (exit code %d)\n", checks[i].name, WEXITSTATUS(st));
    }
    printf("\n%d/%zu checks passed\n", passed, NCHECKS);
    return passed == (int)NCHECKS ? 0 : 1;
}
