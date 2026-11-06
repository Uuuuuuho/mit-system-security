/* loader.c - load an ELF32 (EM_RISCV) executable or a flat binary into RAM.
 *
 * ELF: every PT_LOAD segment is copied to p_vaddr + slide, where
 *      slide = (chosen load base) - (lowest PT_LOAD address, 4 KiB aligned).
 *      The SDK links position-independent executables and its crt0 applies the
 *      R_RISCV_RELATIVE relocations itself, so any 4 KiB-aligned slide works.
 * flat: the file is copied to the load base and execution starts there.
 */
#include <stdlib.h>
#include <string.h>

#include "iss.h"

#define FLAT_RESERVE (1u << 20) /* room assumed for .bss + stack after a flat image */

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} elf32_ehdr;

typedef struct {
    uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} elf32_phdr;

static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    unsigned char *buf = malloc(cap);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        size_t r = fread(buf + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }
    fclose(f);
    *len = n;
    return buf;
}

static int is_elf(const unsigned char *b, size_t n) { return n >= sizeof(elf32_ehdr) && !memcmp(b, "\177ELF", 4); }

static int elf_check(const unsigned char *b, size_t n, const char *path) {
    const elf32_ehdr *h = (const elf32_ehdr *)b;
    if (b[4] != 1 || b[5] != 1 || h->e_machine != 243) {
        fprintf(stderr, "psp-lite: %s: not a 32-bit little-endian RISC-V ELF\n", path);
        return -1;
    }
    if (h->e_phentsize != sizeof(elf32_phdr) || h->e_phoff + (size_t)h->e_phnum * sizeof(elf32_phdr) > n) {
        fprintf(stderr, "psp-lite: %s: bad program headers\n", path);
        return -1;
    }
    return 0;
}

static void elf_range(const unsigned char *b, uint32_t *lo, uint32_t *hi) {
    const elf32_ehdr *h = (const elf32_ehdr *)b;
    *lo = 0xFFFFFFFFu;
    *hi = 0;
    for (int i = 0; i < h->e_phnum; i++) {
        elf32_phdr ph;
        memcpy(&ph, b + h->e_phoff + i * sizeof ph, sizeof ph);
        if (ph.p_type != 1 || ph.p_memsz == 0) continue;
        if (ph.p_vaddr < *lo) *lo = ph.p_vaddr;
        if (ph.p_vaddr + ph.p_memsz > *hi) *hi = ph.p_vaddr + ph.p_memsz;
    }
    *lo &= ~0xFFFu;
}

int image_span(const char *path, int force_flat, uint32_t *span_out, int *elf_out) {
    size_t n;
    unsigned char *b = slurp(path, &n);
    if (!b) {
        perror(path);
        return -1;
    }
    int elf = !force_flat && is_elf(b, n);
    if (elf) {
        if (elf_check(b, n, path)) {
            free(b);
            return -1;
        }
        uint32_t lo, hi;
        elf_range(b, &lo, &hi);
        *span_out = hi > lo ? hi - lo : 0;
    } else {
        *span_out = (uint32_t)n + FLAT_RESERVE;
    }
    *elf_out = elf;
    free(b);
    return 0;
}

/* base == 0xFFFFFFFF means "use the link address" (ELF) / RAM base (flat). */
int load_image(psp_cpu *c, const char *path, int force_flat, uint32_t base, uint32_t *entry_out) {
    size_t n;
    unsigned char *b = slurp(path, &n);
    if (!b) {
        perror(path);
        return -1;
    }
    if (!force_flat && is_elf(b, n)) {
        if (elf_check(b, n, path)) goto fail;
        const elf32_ehdr *h = (const elf32_ehdr *)b;
        uint32_t lo, hi;
        elf_range(b, &lo, &hi);
        if (base == 0xFFFFFFFFu) {
            /* not linked into RAM? then place it at the bottom of RAM */
            base = (lo - c->ram_base < c->ram_size) ? lo : c->ram_base;
        }
        uint32_t slide = base - lo;
        for (int i = 0; i < h->e_phnum; i++) {
            elf32_phdr ph;
            memcpy(&ph, b + h->e_phoff + i * sizeof ph, sizeof ph);
            if (ph.p_type != 1 || ph.p_memsz == 0) continue;
            uint32_t dst = ph.p_vaddr + slide;
            uint32_t off = dst - c->ram_base;
            if (off >= c->ram_size || ph.p_memsz > c->ram_size - off || ph.p_filesz > ph.p_memsz ||
                (size_t)ph.p_offset + ph.p_filesz > n) {
                fprintf(stderr, "psp-lite: %s: segment at %08x (+%u bytes) does not fit in RAM %08x-%08x\n", path,
                        dst, ph.p_memsz, c->ram_base, c->ram_base + c->ram_size - 1);
                goto fail;
            }
            memcpy(c->ram + off, b + ph.p_offset, ph.p_filesz);
            memset(c->ram + off + ph.p_filesz, 0, ph.p_memsz - ph.p_filesz);
        }
        c->load_base = base;
        *entry_out = h->e_entry + slide;
    } else {
        if (base == 0xFFFFFFFFu) base = c->ram_base;
        uint32_t off = base - c->ram_base;
        if (off >= c->ram_size || n > c->ram_size - off) {
            fprintf(stderr, "psp-lite: %s: flat image (%zu bytes) does not fit at %08x\n", path, n, base);
            goto fail;
        }
        memcpy(c->ram + off, b, n);
        c->load_base = base;
        *entry_out = base;
    }
    free(b);
    return 0;
fail:
    free(b);
    return -1;
}
