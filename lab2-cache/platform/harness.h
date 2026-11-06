/* harness.h - driver/victim side of the platform. NOT for attack code.
 *
 * Student files (everything under partN/) must not call anything declared here; the grader rejects objects that
 * reference hx_* symbols. Victims and the per-part drivers in drivers/ use it to create parties,
 * shared mappings and secrets.
 */
#ifndef HARNESS_H
#define HARNESS_H
#include <stddef.h>
#include <stdint.h>
#include "plat.h"

/* Where a new party runs, relative to the main (attacker) party which sits on core 0, hart 0. */
typedef enum {
    HX_HART0,      /* core 0, hart 0 (use only when main itself just waits in hx_join)          */
    HX_SIBLING,    /* core 0, hart 1: shares L1+L2 with main (SMT sibling; native: same CPU)    */
    HX_OTHER_CORE  /* core 1: shares only the LLC with main                                     */
} hx_place_t;

/* Parse --seed N (or SEED=...), select the backend (SHD_BACKEND=sim|native), start the platform. */
void hx_init(int argc, char **argv, const char *part_name);

/* Value of "--name value" on the command line, or NULL. hx_flag("--name") tests a bare flag. */
const char *hx_arg(const char *name);
int hx_flag(const char *name);

uint64_t hx_seed(void);
uint64_t hx_secret_rand(void);   /* independent stream for secrets */

/* Start a party running fn(arg). Returns an id for hx_join. The party runs until fn returns,
 * calls hx_return(), or the program ends (victims usually loop forever). */
int hx_spawn(void (*fn)(void *), void *arg, hx_place_t where, const char *name);

/* Inside a spawned party: hand `len` bytes back to whoever joins, then terminate the party. */
void hx_return(const void *data, size_t len);

/* In main: wait for party `id` to finish; copies its hx_return payload (NUL-terminated) to out.
 * Returns the payload length, or -1 on failure. */
int hx_join(int id, char *out, size_t cap);

/* A read-only mapping shared by every party (native: memfd mapped MAP_SHARED, like a library). */
const uint8_t *hx_shared_buf(size_t bytes);

/* Physical address of p (sim: model PA; native: /proc/self/pagemap, needs root). ~0 if unknown. */
uint64_t hx_pa_of(const void *p);

/* Make plat_flush() abort (Part 5: no flush instruction allowed). */
void hx_disable_flush(void);

/* Record the sequence of cache lines touched by the calling party (sim only; native returns -1). */
void hx_trace_start(void);
int  hx_trace_stop(uint64_t *lines, int cap);

/* Abort the run (RESULT FAIL timeout) after this many seconds of wall time / virtual ticks. */
void hx_set_timeout(unsigned wall_seconds, uint64_t virtual_ticks);

/* Print the final verdict line parsed by check.py, then exit(0). */
void hx_result(int pass, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));

#endif
