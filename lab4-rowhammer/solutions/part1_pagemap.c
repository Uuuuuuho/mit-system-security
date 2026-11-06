/* part1_pagemap.c - REFERENCE SOLUTION for Part 1. SPOILER: try the lab yourself first.
 *
 * Exercise 1-1  pm_decode / pm_virt_to_phys
 * Exercise 1-2  revmap_build / revmap_phys_to_virt
 * Exercise 1-3  revmap_count_huge
 */
#include <stdlib.h>
#include <string.h>

#include "../include/lab4.h"

/* /proc/self/pagemap entry format (Documentation/admin-guide/mm/pagemap.rst):
 *   bits 0-54  PFN if present (zero for unprivileged readers since Linux 4.0)
 *   bit  55    soft-dirty      bit 56  exclusively mapped   bit 57 uffd-wp
 *   bit  61    file page or shared anon                     bit 62 swapped    bit 63 present */
#define PM_PRESENT (1ull << 63)
#define PM_SWAPPED (1ull << 62)
#define PM_PFN_MASK ((1ull << 55) - 1)

int pm_decode(uint64_t entry, uint64_t vaddr, size_t page_size, uint64_t *paddr) {
    if (!(entry & PM_PRESENT) || (entry & PM_SWAPPED)) return -1;
    uint64_t pfn = entry & PM_PFN_MASK;
    if (pfn == 0) return -1; /* hidden: no CAP_SYS_ADMIN */
    *paddr = pfn * page_size + (vaddr % page_size);
    return 0;
}

int pm_virt_to_phys(pagemap_t *pm, const void *vaddr, uint64_t *paddr) {
    uint64_t va = (uint64_t)(uintptr_t)vaddr, e;
    if (pagemap_read(pm, va, &e) != 0) return -1;
    return pm_decode(e, va, pm_page_size(), paddr);
}

typedef struct {
    uint64_t pa;   /* physical address of the page */
    uintptr_t va;  /* virtual address of the page */
} pair_t;

struct revmap {
    uintptr_t buf;
    size_t len, ps, hs;
    size_t n;          /* translated pages */
    pair_t *by_pa;     /* sorted by physical address */
    uint64_t *pa_of;   /* per virtual page, 0 = no frame */
};

static int cmp_pa(const void *x, const void *y) {
    uint64_t a = ((const pair_t *)x)->pa, b = ((const pair_t *)y)->pa;
    return (a > b) - (a < b);
}

revmap_t *revmap_build(pagemap_t *pm, void *buf, size_t len) {
    revmap_t *rm = (revmap_t *)calloc(1, sizeof *rm);
    if (!rm) return NULL;
    rm->buf = (uintptr_t)buf;
    rm->len = len;
    rm->ps = pm_page_size();
    rm->hs = pm_huge_size();
    size_t np = len / rm->ps;
    rm->by_pa = (pair_t *)malloc(np * sizeof(pair_t));
    rm->pa_of = (uint64_t *)calloc(np, sizeof(uint64_t));
    if (!rm->by_pa || !rm->pa_of) { revmap_free(rm); return NULL; }
    for (size_t i = 0; i < np; i++) {
        uintptr_t va = rm->buf + i * rm->ps;
        uint64_t pa;
        if (pm_virt_to_phys(pm, (const void *)va, &pa) != 0) continue;
        rm->pa_of[i] = pa;
        rm->by_pa[rm->n].pa = pa;
        rm->by_pa[rm->n].va = va;
        rm->n++;
    }
    qsort(rm->by_pa, rm->n, sizeof(pair_t), cmp_pa);
    return rm;
}

void *revmap_phys_to_virt(const revmap_t *rm, uint64_t paddr) {
    uint64_t page = paddr & ~(uint64_t)(rm->ps - 1);
    size_t lo = 0, hi = rm->n;
    while (lo < hi) { /* binary search */
        size_t mid = lo + (hi - lo) / 2;
        if (rm->by_pa[mid].pa < page) lo = mid + 1;
        else hi = mid;
    }
    if (lo < rm->n && rm->by_pa[lo].pa == page) return (void *)(rm->by_pa[lo].va + (paddr - page));
    return NULL;
}

size_t revmap_count_huge(const revmap_t *rm) {
    size_t per = rm->hs / rm->ps, count = 0;
    uintptr_t first = (rm->buf + rm->hs - 1) & ~(uintptr_t)(rm->hs - 1);
    for (uintptr_t c = first; c + rm->hs <= rm->buf + rm->len; c += rm->hs) {
        size_t i0 = (c - rm->buf) / rm->ps;
        uint64_t base = rm->pa_of[i0];
        int ok = base != 0 && base % rm->hs == 0;
        for (size_t i = 1; i < per && ok; i++) ok = rm->pa_of[i0 + i] == base + i * rm->ps;
        count += (size_t)ok;
    }
    return count;
}

void revmap_free(revmap_t *rm) {
    if (!rm) return;
    free(rm->by_pa);
    free(rm->pa_of);
    free(rm);
}
