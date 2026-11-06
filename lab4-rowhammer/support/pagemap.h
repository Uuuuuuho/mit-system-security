/* pagemap.h - access to page-table entries of THIS process, native or simulated (Part 1).
 *
 * Linux exposes one 64-bit entry per virtual page in /proc/self/pagemap (see the kernel's
 * Documentation/admin-guide/mm/pagemap.rst). Since Linux 4.0 the physical frame number (PFN)
 * field reads as zero unless the reader has CAP_SYS_ADMIN, precisely because of Rowhammer.
 *
 * This layer gives you the RAW entries and leaves the decoding to you (Exercise 1-1):
 *   native mode : entries are read from /proc/self/pagemap with pread().
 *   sim mode    : entries have exactly the same format, but PFNs come from a simulated
 *                 "kernel" that owns 16 GiB of fake physical memory. The present bit is still
 *                 taken from the real kernel, so untouched pages really are "not present".
 *                 Buffers obtained from pagemap_alloc(.., huge=1) are backed by fake huge pages
 *                 (physically contiguous, huge-page aligned); everything else gets scattered
 *                 frames; pages that pagemap_alloc() did not hand out report PFN 0 (as for an
 *                 unprivileged reader).
 * pagemap_open(PM_AUTO, ..) picks native when PFNs are visible and falls back to sim otherwise.
 * The environment variable SHD_PAGEMAP=native|sim|auto overrides PM_AUTO.
 */
#ifndef LAB4_PAGEMAP_H
#define LAB4_PAGEMAP_H
#include <stddef.h>
#include <stdint.h>

typedef enum { PM_AUTO = 0, PM_NATIVE = 1, PM_SIM = 2 } pm_mode_t;
typedef struct pagemap pagemap_t;

pagemap_t *pagemap_open(pm_mode_t want, uint64_t seed);
void pagemap_close(pagemap_t *pm);
pm_mode_t pagemap_mode(const pagemap_t *pm);
const char *pagemap_mode_name(const pagemap_t *pm);
const char *pagemap_reason(const pagemap_t *pm); /* why this mode was chosen */

size_t pm_page_size(void); /* base page size (4 KiB on most systems, 16/64 KiB on some arm64) */
size_t pm_huge_size(void); /* PMD-level huge page size (2 MiB with 4 KiB pages) */

/* Raw pagemap entry of the page that contains vaddr. Returns 0 on success, -1 on error. */
int pagemap_read(pagemap_t *pm, uint64_t vaddr, uint64_t *entry);

/* Allocate `bytes` (rounded up to whole pages) of populated, writable memory. With huge=1 the
 * buffer is huge-page aligned and the allocator asks for huge pages (hugetlbfs if configured,
 * else transparent huge pages); in sim mode it is backed by fake huge pages. Never freed. */
void *pagemap_alloc(pagemap_t *pm, size_t bytes, int huge);

#endif
