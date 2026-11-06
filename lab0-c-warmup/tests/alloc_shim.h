/* alloc_shim.h - a tiny allocation tracker used by the tester (not part of the library).
 *
 * The tester is linked with  -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
 * so every malloc/calloc/realloc/free call made by logbook.c and tester.c goes through the
 * __wrap_* functions in alloc_shim.c, which
 *   - keep a table of live blocks (pointer -> size), so the tester can detect leaks
 *     deterministically, without needing valgrind or LeakSanitizer;
 *   - abort with a clear message on free() of a pointer that is not live (double/invalid free);
 *   - fill fresh malloc()/realloc() memory with 0xA5 garbage (like a debug allocator), so reads
 *     of uninitialized fields misbehave reliably instead of "happening to be 0";
 *     set SHIM_NOFILL=1 to disable (the valgrind run does, so valgrind can see the
 *     uninitialized bytes itself);
 *   - can make the next allocation fail (fault injection for out-of-memory paths).
 */
#ifndef ALLOC_SHIM_H
#define ALLOC_SHIM_H
#include <stddef.h>

/* Number of live blocks / bytes allocated through the shim. */
size_t shim_live_blocks(void);
size_t shim_live_bytes(void);

/* Make allocation number `n` from now fail (0 = the very next malloc/calloc/realloc returns
 * NULL). Only one failure is injected. shim_fail_after(-1) cancels. */
void shim_fail_after(long n);

/* Number of failures injected so far. */
size_t shim_failures(void);

/* Print the live blocks (size and allocating code address) to stdout, at most `max` lines. */
void shim_dump_live(int max);

#endif
