/* part5_main.c - harness for Part 5 (SECDED Hamming(39,32)). Provided; do not edit.
 *
 *   ./build/part5 [--seed N] [-v]
 * Unit tests of your encoder/decoder, then a demo: ECC-protected words in victim rows of the
 * simulated module are hammered (with the harness's own double-sided hammer) and decoded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcommon.h"

#define CW_MASK ((1ull << 39) - 1)

static const char *st_name(int s) {
    return s == ECC_OK ? "ok" : s == ECC_CORRECTED ? "corrected" : s == ECC_UNCORRECTABLE ? "detected" : "bad-status";
}

int main(int argc, char **argv) {
    hargs_t a;
    hargs_parse(argc, argv, &a, NULL);
    hres_t r = {5, 0, 0};
    uint64_t rs = a.seed ^ 0x5555;
    printf("== Part 5: SECDED Hamming(39,32)\n");

    /* ---- unit tests ---- */
    {
        int good = 0, n = 0;
        uint32_t edge[] = {0, 0xFFFFFFFFu, 1, 0x80000000u, 0xAAAAAAAAu, 0x55555555u};
        for (int i = 0; i < 20000 + 6; i++) {
            uint32_t d = i < 6 ? edge[i] : (uint32_t)hrand(&rs), out = ~d;
            uint64_t cw = secded_encode(d);
            int stt = secded_decode(cw, &out);
            n++;
            good += (cw & ~CW_MASK) == 0 && stt == ECC_OK && out == d;
        }
        hcheck(&r, good == n, "round trip, codeword fits in 39 bits", "%d/%d", good, n);
    }
    {
        int good = 0, n = 0;
        for (int i = 0; i < 2000; i++) {
            uint32_t d = (uint32_t)hrand(&rs);
            uint64_t cw = secded_encode(d);
            for (int b = 0; b < 39; b++) {
                uint32_t out = ~d;
                int stt = secded_decode(cw ^ (1ull << b), &out);
                n++;
                good += stt == ECC_CORRECTED && out == d;
                if (!(stt == ECC_CORRECTED && out == d) && a.verbose && good + 5 > n)
                    printf("    data %08x bit %d: %s, data %08x\n", d, b, st_name(stt), out);
            }
        }
        hcheck(&r, good == n, "every single-bit error corrected", "%d/%d", good, n);
    }
    {
        int good = 0, n = 0;
        for (int i = 0; i < 200; i++) {
            uint32_t d = (uint32_t)hrand(&rs), out = ~d;
            uint64_t cw = secded_encode(d);
            int base_ok = secded_decode(cw, &out) == ECC_OK && out == d; /* no credit for "always fail" */
            for (int b1 = 0; b1 < 39; b1++)
                for (int b2 = b1 + 1; b2 < 39; b2++) {
                    n++;
                    good += base_ok && secded_decode(cw ^ (1ull << b1) ^ (1ull << b2), &out) == ECC_UNCORRECTABLE;
                }
        }
        hcheck(&r, good == n, "every double-bit error detected", "%d/%d", good, n);
    }
    {
        int good = 0, n = 2000;
        for (int i = 0; i < n; i++) {
            uint32_t d = (uint32_t)hrand(&rs), out = ~d;
            uint64_t junk = hrand(&rs) & ~CW_MASK;
            int stt = secded_decode(secded_encode(d) ^ junk, &out);
            good += stt == ECC_OK && out == d;
        }
        hcheck(&r, good == n, "bits above bit 38 ignored", "%d/%d", good, n);
    }
    {
        int det = 0, mis = 0, n = 20000;
        for (int i = 0; i < n; i++) {
            uint32_t d = (uint32_t)hrand(&rs), out;
            uint64_t e = 0;
            while (__builtin_popcountll(e) < 3) e |= 1ull << (hrand(&rs) % 39);
            int stt = secded_decode(secded_encode(d) ^ e, &out);
            if (stt == ECC_UNCORRECTABLE) det++;
            else if (out != d) mis++;
        }
        printf("  triple-bit errors: %.1f%% detected, %.1f%% silently MIS-corrected (%d trials)\n", 100.0 * det / n,
               100.0 * mis / n, n);
    }

    /* ---- demo: ECC words under a double-sided hammer in the simulated module ---- */
    {
        dsim_t *s = hsim(a.seed, 0);
        const dsim_info_t *in = dsim_info(s);
        unsigned first, count;
        hbuf_rows(s, &first, &count);
        unsigned words = in->row_bytes / 8;
        uint32_t *orig = (uint32_t *)malloc(sizeof(uint32_t) * words * 3);
        long hist[6][4];
        memset(hist, 0, sizeof hist);
        long wrong = 0, words_total = 0, pad_flips = 0;
        for (int loc = 0; loc < 24; loc++) {
            unsigned bank = (unsigned)(hrand(&rs) % in->nbanks);
            unsigned row = first + 2 + (unsigned)(hrand(&rs) % (count - 8));
            unsigned aggr[2] = {row, row + 2}, vict[3] = {row - 1, row + 1, row + 3};
            /* victims: codewords of random data; aggressors: complement of the middle victim */
            for (int v = 0; v < 3; v++)
                for (unsigned w = 0; w < words; w++) {
                    uint32_t d = (uint32_t)hrand(&rs);
                    orig[v * words + w] = d;
                    dsim_write64(s, dsim_oracle_compose(s, bank, vict[v], w * 8), secded_encode(d));
                }
            for (int k = 0; k < 2; k++)
                for (unsigned w = 0; w < words; w++)
                    dsim_write64(s, dsim_oracle_compose(s, bank, aggr[k], w * 8), ~secded_encode(orig[1 * words + w]));
            href_hammer(s, bank, aggr, 2, 2.0 * in->tREFW_ns);
            for (int v = 0; v < 3; v++)
                for (unsigned w = 0; w < words; w++) {
                    uint32_t d = orig[v * words + w], out = ~d;
                    uint64_t good_cw = secded_encode(d);
                    uint64_t raw = dsim_read64(s, dsim_oracle_compose(s, bank, vict[v], w * 8));
                    int nf = __builtin_popcountll((raw ^ good_cw) & CW_MASK);
                    pad_flips += __builtin_popcountll((raw ^ good_cw) & ~CW_MASK);
                    int stt = secded_decode(raw, &out);
                    int col = stt == ECC_UNCORRECTABLE ? 2 : (out == d ? (nf ? 1 : 0) : 3);
                    hist[nf < 5 ? nf : 5][col]++;
                    words_total++;
                    if ((nf == 0 && !(stt == ECC_OK && out == d)) || (nf == 1 && !(stt == ECC_CORRECTED && out == d)) ||
                        (nf == 2 && stt != ECC_UNCORRECTABLE))
                        wrong++;
                }
        }
        printf("\n  hammered %ld protected words (24 locations x 3 victim rows); %ld flips hit padding bits\n", words_total,
               pad_flips);
        printf("  %-12s %10s %10s %10s %14s\n", "bit errors", "ok", "corrected", "detected", "WRONG DATA");
        for (int k = 0; k < 6; k++) {
            if (!hist[k][0] && !hist[k][1] && !hist[k][2] && !hist[k][3]) continue;
            printf("  %-12s %10ld %10ld %10ld %14ld\n", k == 0 ? "0" : k == 1 ? "1" : k == 2 ? "2" : k == 3 ? "3" : k == 4 ? "4" : "5+",
                   hist[k][0], hist[k][1], hist[k][2], hist[k][3]);
        }
        hcheck(&r, wrong == 0, "decoder behaves on hammered words", "%ld word(s) mishandled", wrong);
        free(orig);
        dsim_destroy(s);
    }
    return hfinish(&r);
}
