/* pagemap.c - native and simulated /proc/self/pagemap backends (see pagemap.h). Provided code. */
#define _GNU_SOURCE
#include "pagemap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SIM_PHYS_BYTES (16ull << 30) /* fake physical memory of the simulated kernel */
#define MAX_REGIONS 64
#define PFN_FIELD ((1ull << 55) - 1)

typedef struct {
    uintptr_t start;
    size_t len;
    int huge;
    uint64_t *pfn; /* one fake PFN per base page */
} region_t;

struct pagemap {
    pm_mode_t mode;
    int fd;
    char reason[192];
    uint64_t rng;
    region_t reg[MAX_REGIONS];
    int nreg;
};

static uint64_t pm_rand(pagemap_t *pm) {
    uint64_t z = (pm->rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

size_t pm_page_size(void) {
    static size_t ps;
    if (!ps) { long v = sysconf(_SC_PAGESIZE); ps = v > 0 ? (size_t)v : 4096; }
    return ps;
}

size_t pm_huge_size(void) {
    static size_t hs;
    if (!hs) {
        FILE *f = fopen("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size", "r");
        unsigned long long v = 0;
        if (f) { if (fscanf(f, "%llu", &v) != 1) v = 0; fclose(f); }
        /* a PMD maps (page_size / 8) base pages */
        hs = v ? (size_t)v : pm_page_size() * (pm_page_size() / 8);
    }
    return hs;
}

static int read_real(pagemap_t *pm, uint64_t vaddr, uint64_t *e) {
    if (pm->fd < 0) return -1;
    off_t off = (off_t)(vaddr / pm_page_size()) * 8;
    return pread(pm->fd, e, 8, off) == 8 ? 0 : -1;
}

pagemap_t *pagemap_open(pm_mode_t want, uint64_t seed) {
    pagemap_t *pm = (pagemap_t *)calloc(1, sizeof *pm);
    if (!pm) return NULL;
    pm->rng = seed ^ 0x7061676D6170ull;
    pm->fd = open("/proc/self/pagemap", O_RDONLY);
    int open_errno = errno;

    if (want == PM_AUTO) {
        const char *env = getenv("SHD_PAGEMAP");
        if (env && strcmp(env, "native") == 0) want = PM_NATIVE;
        else if (env && strcmp(env, "sim") == 0) want = PM_SIM;
    }
    if (want == PM_SIM) {
        pm->mode = PM_SIM;
        snprintf(pm->reason, sizeof pm->reason, "simulated pagemap requested");
        return pm;
    }
    if (want == PM_NATIVE) {
        if (pm->fd < 0) {
            fprintf(stderr, "pagemap: cannot open /proc/self/pagemap: %s\n", strerror(open_errno));
            free(pm);
            return NULL;
        }
        pm->mode = PM_NATIVE;
        snprintf(pm->reason, sizeof pm->reason, "native pagemap requested");
        return pm;
    }
    /* PM_AUTO: probe whether this process can see PFNs */
    pm->mode = PM_SIM;
    if (pm->fd < 0) {
        snprintf(pm->reason, sizeof pm->reason, "cannot open /proc/self/pagemap (%s)", strerror(open_errno));
        return pm;
    }
    volatile uint8_t *probe = (volatile uint8_t *)mmap(NULL, pm_page_size(), PROT_READ | PROT_WRITE,
                                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (probe == (void *)MAP_FAILED) {
        snprintf(pm->reason, sizeof pm->reason, "mmap failed");
        return pm;
    }
    probe[0] = 1;
    uint64_t e = 0;
    if (read_real(pm, (uint64_t)(uintptr_t)probe, &e) != 0)
        snprintf(pm->reason, sizeof pm->reason, "reading /proc/self/pagemap failed");
    else if (!((e >> 63) & 1))
        snprintf(pm->reason, sizeof pm->reason, "probe page reported not present");
    else if ((e & PFN_FIELD) == 0)
        snprintf(pm->reason, sizeof pm->reason, "PFNs read as zero (no CAP_SYS_ADMIN?)");
    else {
        pm->mode = PM_NATIVE;
        snprintf(pm->reason, sizeof pm->reason, "PFNs are visible");
    }
    munmap((void *)probe, pm_page_size());
    return pm;
}

void pagemap_close(pagemap_t *pm) {
    if (!pm) return;
    if (pm->fd >= 0) close(pm->fd);
    for (int i = 0; i < pm->nreg; i++) free(pm->reg[i].pfn);
    free(pm);
}

pm_mode_t pagemap_mode(const pagemap_t *pm) { return pm->mode; }
const char *pagemap_mode_name(const pagemap_t *pm) { return pm->mode == PM_NATIVE ? "native" : "sim"; }
const char *pagemap_reason(const pagemap_t *pm) { return pm->reason; }

/* ---------- the simulated kernel's frame allocator ----------
 * Frames are drawn at random from SIM_PHYS_BYTES of fake memory; unassigned slots are 0. */
static int frame_in_use(const pagemap_t *pm, uint64_t lo, uint64_t n) {
    for (int r = 0; r < pm->nreg; r++) {
        const region_t *g = &pm->reg[r];
        size_t np = g->len / pm_page_size();
        for (size_t i = 0; i < np; i++)
            if (g->pfn[i] && g->pfn[i] >= lo && g->pfn[i] < lo + n) return 1;
    }
    return 0;
}

static void sim_assign(pagemap_t *pm, region_t *g) {
    size_t ps = pm_page_size(), np = g->len / ps;
    uint64_t nframes = SIM_PHYS_BYTES / ps;
    uint64_t per_huge = pm_huge_size() / ps;
    if (g->huge) {
        for (size_t c = 0; c < np; c += per_huge) {
            uint64_t base;
            do base = (1 + pm_rand(pm) % (nframes / per_huge - 1)) * per_huge;
            while (frame_in_use(pm, base, per_huge));
            for (uint64_t i = 0; i < per_huge && c + i < np; i++) g->pfn[c + i] = base + i;
        }
    } else {
        for (size_t i = 0; i < np; i++) {
            uint64_t f;
            int clash;
            do {
                f = 1 + pm_rand(pm) % (nframes - 1);
                clash = frame_in_use(pm, f, 1);
            } while (clash);
            g->pfn[i] = f;
        }
    }
}

static uint64_t sim_lookup(const pagemap_t *pm, uint64_t vaddr) {
    for (int r = 0; r < pm->nreg; r++) {
        const region_t *g = &pm->reg[r];
        if (vaddr >= g->start && vaddr < g->start + g->len) return g->pfn[(vaddr - g->start) / pm_page_size()];
    }
    return 0;
}

int pagemap_read(pagemap_t *pm, uint64_t vaddr, uint64_t *entry) {
    if (pm->mode == PM_NATIVE) return read_real(pm, vaddr, entry);

    uint64_t real = 0;
    int present;
    if (read_real(pm, vaddr, &real) == 0) {
        present = (int)((real >> 63) & 1);
    } else {
        /* no pagemap at all: ask mincore() whether the page is resident */
        unsigned char vec = 0;
        void *pg = (void *)(uintptr_t)(vaddr & ~(uint64_t)(pm_page_size() - 1));
        present = (mincore(pg, pm_page_size(), &vec) == 0) && (vec & 1);
        real = present ? (1ull << 63) : 0;
    }
    if (!present) { *entry = real; return 0; }
    uint64_t pfn = sim_lookup(pm, vaddr);
    *entry = (real & ~PFN_FIELD) | pfn;
    if (pfn) *entry |= (1ull << 56) | (1ull << 55); /* exclusively mapped + soft-dirty */
    return 0;
}

void *pagemap_alloc(pagemap_t *pm, size_t bytes, int huge) {
    size_t ps = pm_page_size(), hs = pm_huge_size();
    size_t unit = huge ? hs : ps;
    size_t len = (bytes + unit - 1) / unit * unit;
    void *p = MAP_FAILED;
    if (huge) {
#ifdef MAP_HUGETLB
        if (pm->mode == PM_NATIVE)
            p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
#endif
        if (p == MAP_FAILED) {
            /* over-allocate, trim to a huge-aligned window, ask for transparent huge pages */
            uint8_t *raw = (uint8_t *)mmap(NULL, len + hs, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (raw == (uint8_t *)MAP_FAILED) { perror("pagemap_alloc: mmap"); return NULL; }
            uintptr_t a = ((uintptr_t)raw + hs - 1) & ~(uintptr_t)(hs - 1);
            if (a > (uintptr_t)raw) munmap(raw, a - (uintptr_t)raw);
            uintptr_t end = (uintptr_t)raw + len + hs;
            if (end > a + len) munmap((void *)(a + len), end - (a + len));
            p = (void *)a;
#ifdef MADV_HUGEPAGE
            madvise(p, len, MADV_HUGEPAGE);
#endif
        }
    } else {
        p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (p == MAP_FAILED) { perror("pagemap_alloc: mmap"); return NULL; }
    }
    memset(p, 0x5A, len); /* fault every page in */
    if (pm->mode == PM_SIM) {
        if (pm->nreg >= MAX_REGIONS) { fprintf(stderr, "pagemap_alloc: too many regions\n"); return p; }
        region_t *g = &pm->reg[pm->nreg];
        g->start = (uintptr_t)p;
        g->len = len;
        g->huge = huge;
        g->pfn = (uint64_t *)calloc(len / ps, sizeof(uint64_t));
        if (!g->pfn) { perror("pagemap_alloc: calloc"); return p; }
        pm->nreg++; /* register first so frame_in_use() also sees this region's frames */
        sim_assign(pm, g);
    }
    return p;
}
