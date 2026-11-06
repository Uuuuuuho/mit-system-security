/* part1_main.c - harness for Part 1 (virt -> phys -> virt). Provided; do not edit.
 *
 *   ./build/part1 [--mode auto|native|sim] [--seed N] [-v]
 *
 * The checks compare your functions with an independent decoder that lives in this file.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "hcommon.h"

static int ref_decode(uint64_t e, uint64_t va, size_t ps, uint64_t *pa) {
    if (!((e >> 63) & 1) || ((e >> 62) & 1)) return -1;
    uint64_t pfn = e & ((1ull << 55) - 1);
    if (!pfn) return -1;
    *pa = pfn * ps + (va % ps);
    return 0;
}

static int ref_translate(pagemap_t *pm, uint64_t va, uint64_t *pa) {
    uint64_t e;
    if (pagemap_read(pm, va, &e) != 0) return -1;
    return ref_decode(e, va, pm_page_size(), pa);
}

int main(int argc, char **argv) {
    hargs_t a;
    hargs_parse(argc, argv, &a, " [--mode auto|native|sim]");
    hres_t r = {1, 0, 0};
    uint64_t rs = a.seed ^ 0x1111;
    size_t ps = pm_page_size(), hs = pm_huge_size();
    printf("== Part 1: virtual <-> physical  (page %zu B, huge page %zu KiB)\n", ps, hs >> 10);

    /* ---- 1. pm_decode on hand-made entries ---- */
    {
        struct { uint64_t e, va; int ok; } tc[] = {
            {(1ull << 63) | (1ull << 56) | (1ull << 55) | 0x12345ull, 0x7f0000001abcull, 1}, /* flags around the PFN */
            {(1ull << 63) | (1ull << 61) | 0x00042ull, 0x400123ull, 1},                       /* file-backed page */
            {(1ull << 63) | (1ull << 54) | 0x7ull, 0x1000fffull, 1},                           /* top PFN bit */
            {(1ull << 63) | (1ull << 55), 0x7f0000002000ull, 0},                               /* PFN hidden (zero) */
            {(1ull << 56) | 0x999ull, 0x7f0000003000ull, 0},                                   /* not present */
            {(1ull << 62) | (0x3ull << 5) | 0x2ull, 0x7f0000004000ull, 0},                     /* swapped out */
            {0, 0x7f0000005000ull, 0},                                                          /* nothing */
        };
        int good = 0, n = (int)(sizeof tc / sizeof tc[0]);
        for (int i = 0; i < n; i++) {
            uint64_t want = 0, got = 0xdeadbeef;
            int wok = ref_decode(tc[i].e, tc[i].va, ps, &want) == 0;
            int gok = pm_decode(tc[i].e, tc[i].va, ps, &got) == 0;
            int match = (wok == gok) && (!wok || want == got);
            good += match;
            if (!match || a.verbose)
                printf("    entry %016llx va %llx: expected %s%llx, got %s%llx\n", (unsigned long long)tc[i].e,
                       (unsigned long long)tc[i].va, wok ? "" : "error/", (unsigned long long)want, gok ? "" : "error/",
                       (unsigned long long)got);
        }
        hcheck(&r, good == n, "pm_decode on synthetic entries", "%d/%d", good, n);
    }

    /* ---- 2. open the pagemap ---- */
    pm_mode_t want = PM_AUTO;
    if (!strcmp(a.mode, "native")) want = PM_NATIVE;
    else if (!strcmp(a.mode, "sim")) want = PM_SIM;
    pagemap_t *pm = pagemap_open(want, a.seed);
    if (!pm) {
        hcheck(&r, 0, "pagemap_open", "could not open the requested backend");
        return hfinish(&r);
    }
    printf("  backend: %s (%s)\n", pagemap_mode_name(pm), pagemap_reason(pm));
    printf("BACKEND %s\n", pagemap_mode_name(pm));

    /* ---- 3. translations of a small-page buffer ---- */
    {
        size_t npages = 32;
        uint8_t *buf = (uint8_t *)pagemap_alloc(pm, npages * ps, 0);
        int good = 0, n = 64, valid = 0, shown = 0;
        for (int i = 0; i < n && buf; i++) {
            uint64_t va = (uint64_t)(uintptr_t)(buf + hrand(&rs) % (npages * ps)), want = 0, got = 0;
            int wok = ref_translate(pm, va, &want) == 0;
            int gok = pm_virt_to_phys(pm, (const void *)(uintptr_t)va, &got) == 0;
            valid += wok;
            int match = (wok == gok) && (!wok || want == got);
            good += match;
            if (!match && shown++ < 3)
                printf("    va %llx: expected %s%llx got %s%llx\n", (unsigned long long)va, wok ? "" : "error/",
                       (unsigned long long)want, gok ? "" : "error/", (unsigned long long)got);
        }
        hcheck(&r, buf && good == n && valid == n, "pm_virt_to_phys on a populated buffer", "%d/%d correct", good, n);
        uint64_t probe_pa = 0;
        int can_translate = buf && pm_virt_to_phys(pm, buf, &probe_pa) == 0; /* no credit for "always fail" */

        /* a page that was mapped but never touched is not present */
        uint8_t *untouched = (uint8_t *)mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        uint64_t got = 0, want = 0;
        int wok = ref_translate(pm, (uint64_t)(uintptr_t)untouched, &want) == 0;
        int gok = pm_virt_to_phys(pm, untouched, &got) == 0;
        /* a stack page: native -> valid, sim -> PFN reads as 0 */
        int local = 0;
        uint64_t got2 = 0, want2 = 0;
        int wok2 = ref_translate(pm, (uint64_t)(uintptr_t)&local, &want2) == 0;
        int gok2 = pm_virt_to_phys(pm, &local, &got2) == 0;
        hcheck(&r, can_translate && !wok && !gok && wok2 == gok2 && (!wok2 || want2 == got2), "pages without a usable PFN",
               "untouched page -> %s, stack page -> %s (expected %s)", gok ? "translated?!" : "error",
               gok2 ? "translated" : "error", wok2 ? "translated" : "error");
    }

    /* ---- 4. huge-page buffer: reverse map ---- */
    {
        size_t nch = hs <= (32u << 20) ? 4 : 1, len = nch * hs;
        uint8_t *hb = (uint8_t *)pagemap_alloc(pm, len, 1);
        uint8_t *other = (uint8_t *)pagemap_alloc(pm, 4 * ps, 0);
        /* reference count of contiguous huge chunks */
        size_t ref_huge = 0, per = hs / ps;
        for (size_t c = 0; hb && c < nch; c++) {
            uint64_t base = 0;
            int ok = 1;
            for (size_t i = 0; i < per && ok; i++) {
                uint64_t pa;
                if (ref_translate(pm, (uint64_t)(uintptr_t)(hb + c * hs + i * ps), &pa) != 0) { ok = 0; break; }
                if (i == 0) { base = pa; ok = (pa % hs) == 0; }
                else ok = (pa == base + i * ps);
            }
            ref_huge += ok;
        }
        printf("  huge buffer: %zu chunk(s) of %zu KiB, %zu physically contiguous (reference)\n", nch, hs >> 10, ref_huge);
        if (pagemap_mode(pm) == PM_SIM && ref_huge != nch)
            printf("  WARNING: simulated huge pages are not contiguous - support library problem\n");

        revmap_t *rm = hb ? revmap_build(pm, hb, len) : NULL;
        hcheck(&r, rm != NULL, "revmap_build", rm ? "built" : "returned NULL");
        int good = 0, n = 2000;
        for (int i = 0; i < n && rm; i++) {
            uint8_t *va = hb + hrand(&rs) % len;
            uint64_t pa;
            if (ref_translate(pm, (uint64_t)(uintptr_t)va, &pa) != 0) continue;
            void *back = revmap_phys_to_virt(rm, pa);
            good += (back == va);
            if (back != va && a.verbose)
                printf("    pa %llx -> %p, expected %p\n", (unsigned long long)pa, back, (void *)va);
        }
        hcheck(&r, rm && good == n, "revmap_phys_to_virt round trip", "%d/%d", good, n);
        int neg_ok = 1;
        for (int i = 0; i < 4 && rm && other; i++) {
            uint64_t pa;
            if (ref_translate(pm, (uint64_t)(uintptr_t)(other + i * ps + 8), &pa) == 0 && revmap_phys_to_virt(rm, pa))
                neg_ok = 0;
        }
        if (rm && revmap_phys_to_virt(rm, 0)) neg_ok = 0;
        hcheck(&r, rm && neg_ok, "revmap_phys_to_virt outside the buffer", neg_ok ? "NULL as expected" : "returned a pointer");
        size_t got = rm ? revmap_count_huge(rm) : (size_t)-1;
        hcheck(&r, rm && got == ref_huge, "revmap_count_huge", "%zu (expected %zu)", got, ref_huge);
        if (ref_huge)
            printf("  -> inside a contiguous huge page the low %d physical-address bits equal the virtual ones\n",
                   __builtin_ctzll((unsigned long long)hs));
        if (rm) revmap_free(rm);
    }
    pagemap_close(pm);
    return hfinish(&r);
}
