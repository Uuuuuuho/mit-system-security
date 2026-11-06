/* extra_tests.c - Challenge 0-4: your own tests for the logbook library (not graded).
 *
 *   make extra      builds this file together with src/logbook.c under ASan + UBSan and runs it
 *
 * Ideas:
 *   - edge cases tester.c does not cover: removing the only entry, editing an entry with its own
 *     text (lb_edit(lb, s, lb_find(lb, s)->text)), lb_reserve() to a smaller capacity, tags
 *     65535, very long texts, lb_mark() with 0 ...
 *   - a randomized differential test: perform thousands of random operations on a logbook and
 *     on a trivial reference model (e.g. fixed arrays), compare after every step. This is the
 *     idea behind the CPU fuzzing lab (lab6), applied to a C library.
 *   - re-introduce one of the bugs you fixed and check that your test catches it.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logbook.h"

static int tests_run, tests_failed;
#define EXPECT(cond)                                                                       \
    do {                                                                                   \
        tests_run++;                                                                       \
        if (!(cond)) { tests_failed++; printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

/* Example test: removing the only entry leaves an empty, usable logbook. */
static void test_remove_only_entry(void) {
    logbook *lb = lb_create(1);
    EXPECT(lb != NULL);
    uint32_t seq = 0;
    EXPECT(lb_append(&lb, 1, "solo", &seq) == 0);
    EXPECT(lb_remove(lb, seq) == 0);
    EXPECT(lb_count(lb) == 0);
    EXPECT(lb_append(&lb, 2, "again", NULL) == 0);
    EXPECT(lb_count(lb) == 1);
    lb_destroy(lb);
}

/* TODO(Challenge 0-4): add your tests here and call them from main(). */

int main(void) {
    test_remove_only_entry();
    printf("%d/%d expectations passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
