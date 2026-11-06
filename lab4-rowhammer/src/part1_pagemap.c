/* part1_pagemap.c - Part 1: virtual -> physical -> virtual.          (YOUR CODE GOES HERE)
 *
 * Exercise 1-1  pm_decode, pm_virt_to_phys
 * Exercise 1-2  revmap_build, revmap_phys_to_virt
 * Exercise 1-3  revmap_count_huge
 *
 * Build and test:  make && ./build/part1 --mode sim      (simulated pagemap, works everywhere)
 *                  ./build/part1 --mode auto             (native if PFNs are visible)
 * Read support/pagemap.h for the helpers you can use (pagemap_read, pm_page_size, ...), and the
 * kernel's pagemap documentation (Documentation/admin-guide/mm/pagemap.rst) for the entry format.
 */
#include <stdlib.h>
#include <string.h>

#include "../include/lab4.h"

/* ---------------------------------------------------------------- Exercise 1-1 */

int pm_decode(uint64_t entry, uint64_t vaddr, size_t page_size, uint64_t *paddr) {
    /* TODO: a pagemap entry is one 64-bit word per virtual page.
     *  - which bit says "present"? which one says "swapped"?
     *  - which bits hold the page frame number (PFN)? (careful: the flag bits right above the PFN
     *    field are often set, so mask exactly)
     *  - an unprivileged reader sees PFN 0: treat that as failure
     *  - physical address = frame base + offset of vaddr inside its page */
    (void)entry; (void)vaddr; (void)page_size; (void)paddr;
    return -1;
}

int pm_virt_to_phys(pagemap_t *pm, const void *vaddr, uint64_t *paddr) {
    /* TODO: fetch the raw entry with pagemap_read() and decode it with pm_decode(). */
    (void)pm; (void)vaddr; (void)paddr;
    return -1;
}

/* ---------------------------------------------------------------- Exercise 1-2 */

struct revmap {
    /* TODO: whatever you need. Suggestion: the buffer start/length, page and huge-page size, and
     * an array of (physical page, virtual page) pairs sorted by physical address. */
    int unused;
};

revmap_t *revmap_build(pagemap_t *pm, void *buf, size_t len) {
    /* TODO: translate every page of [buf, buf+len) and build a structure that answers
     * "which virtual address maps to this physical address?" quickly (sort + binary search). */
    (void)pm; (void)buf; (void)len;
    return NULL;
}

void *revmap_phys_to_virt(const revmap_t *rm, uint64_t paddr) {
    /* TODO: NULL if paddr is not inside the buffer; otherwise the virtual address (keep the
     * offset within the page!) */
    (void)rm; (void)paddr;
    return NULL;
}

/* ---------------------------------------------------------------- Exercise 1-3 */

size_t revmap_count_huge(const revmap_t *rm) {
    /* TODO: count the huge-page-sized, huge-aligned virtual chunks of the buffer whose base pages
     * are all present, physically contiguous, and start at a huge-aligned physical address. */
    (void)rm;
    return 0;
}

void revmap_free(revmap_t *rm) {
    /* TODO: free everything revmap_build allocated */
    free(rm);
}
