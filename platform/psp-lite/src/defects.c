/* defects.c - seeded "errata" of the psp-lite core.
 *
 * SPOILER WARNING for lab6-cpu-fuzz: this file implements the hidden hardware
 * defects that lab6 asks you to discover by testing the CPU from the inside.
 * Reading it defeats the purpose of the lab.  The parameters are not written
 * here: they are derived at start-up from PSP_SEED, a build-time constant
 * (make SEED=...), so even after reading this file you would not know them for
 * the CPU you are given.  `--golden` (or a DEFECTS=0 build) disables everything.
 */
#include "iss.h"

#ifndef PSP_SEED
#define PSP_SEED 1
#endif
#ifndef PSP_DEFECTS
#define PSP_DEFECTS 1
#endif

/* "calibration" words of the fast-path adder, the custom unit and the decoder */
static uint32_t cw[8];

static uint64_t sm64(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void defects_init(void) {
    uint64_t s = (uint64_t)(PSP_SEED) ^ 0x7073702D6C697465ull;
    uint32_t r[6];
    for (int i = 0; i < 6; i++) r[i] = (uint32_t)(sm64(&s) >> 17);

    /* adder: two nibbles in 1..15 and a bit index in 5..10, packed */
    uint32_t na = 1 + r[0] % 15, nb = 1 + (r[0] >> 8) % 15, fb = 5 + (r[0] >> 16) % 6;
    cw[0] = (na << 4 | nb) ^ 0xA5u;
    cw[1] = ((nb << 4 | na) ^ (na << 4 | nb));
    cw[2] = 1u << fb;
    /* custom unit: one of 1023 (funct7,funct3) pairs and a 20-bit key */
    uint32_t sel = 1 + r[1] % 1023, key = 1 + r[2] % 0xFFFFFu;
    cw[3] = sel * 0x2F0Bu;
    cw[4] = (key ^ 0x0005EED5u) * 0x9E3779B1u;
    /* decoder: which instruction class is mis-decoded, and its parameter */
    static const uint8_t sh[] = {1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15,
                                 17, 18, 19, 20, 21, 22, 23, 25, 26, 27, 28, 29, 30};
    cw[5] = r[3] % 4;
    cw[6] = (cw[5] == 0) ? sh[r[4] % sizeof sh] : (r[4] & 1);
    cw[7] = 0;
}

uint32_t alu_adder(const psp_cpu *c, uint32_t a, uint32_t b) {
    uint32_t s = a + b;
#if PSP_DEFECTS
    /* 16-bit fast path: both operands in [0x100, 0xFFFF] take a shortcut carry-select block */
    if (!c->golden && !((a | b) >> 16) && (a >> 8) && (b >> 8)) {
        uint32_t t = (((a & 15) << 4) | (b & 15)) ^ cw[0] ^ 0xA5u;
        if (t == 0 || t == cw[1]) s ^= cw[2];
    }
#else
    (void)c;
#endif
    return s;
}

int custom0_exec(psp_cpu *c, uint32_t insn, uint32_t *rd_val) {
    uint32_t f3 = (insn >> 12) & 7, f7 = insn >> 25;
    if (f3 == 0 && f7 == 0) { /* psp.id rd : documented, returns the platform id 'PSP1' */
        *rd_val = 0x50535031u;
        return 1;
    }
#if PSP_DEFECTS
    if (!c->golden && ((f7 << 3) | f3) * 0x2F0Bu == cw[3]) {
        if ((c->x[10] ^ 0x0005EED5u) * 0x9E3779B1u == cw[4]) c->priv = PRV_M;
        return 0; /* silently "executes" either way */
    }
#else
    (void)c;
#endif
    return -1;
}

uint32_t decode_fixup(const psp_cpu *c, uint32_t insn, uint32_t golden_result, uint32_t aux) {
#if PSP_DEFECTS
    if (c->golden) return golden_result;
    uint32_t op = insn & 0x7f, f3 = (insn >> 12) & 7;
    switch (cw[5]) {
    case 0: /* SRAI with one particular shamt loses its arithmetic bit */
        if (op == 0x13 && f3 == 5 && (insn >> 25) == 0x20 && ((insn >> 20) & 31) == cw[6]) return aux >> cw[6];
        break;
    case 2: /* SLTIU: immediate zero- instead of sign-extended */
        if (op == 0x13 && f3 == 3 && (insn >> 31)) return aux < ((insn >> 20) & 0xFFFu);
        break;
    case 3: /* LH with a negative offset: sign-extension select taken from the wrong bit */
        if (op == 0x03 && f3 == 1 && (insn >> 31)) return golden_result & 0xFFFFu;
        break;
    default:
        break;
    }
#else
    (void)c;
    (void)insn;
    (void)aux;
#endif
    return golden_result;
}

int x0_write_enable(const psp_cpu *c, uint32_t insn) {
#if PSP_DEFECTS
    if (!c->golden && cw[5] == 1) return (insn & 0x7f) == (cw[6] ? 0x37u : 0x17u);
#else
    (void)c;
    (void)insn;
#endif
    return 0;
}

#ifdef PSP_DEFECTS_DUMP
/* Grader-only: print the derived parameters as JSON (built by `make defects-dump`). */
void defects_dump(FILE *f) {
    uint32_t k = cw[0] ^ 0xA5u, na = k >> 4, nb = k & 15, fb = 0;
    while ((1u << fb) != cw[2]) fb++;
    uint32_t sel = 0;
    for (uint32_t s = 1; s < 1024; s++)
        if (s * 0x2F0Bu == cw[3]) sel = s;
    uint32_t key = 0;
    for (uint32_t x = 1; x < 0x100000u; x++)
        if ((x ^ 0x0005EED5u) * 0x9E3779B1u == cw[4]) key = x;
    static const char *vname[] = {"srai", "x0write", "sltiu", "lh"};
    const char *mn = cw[5] == 0 ? "srai" : cw[5] == 1 ? (cw[6] ? "lui" : "auipc") : cw[5] == 2 ? "sltiu" : "lh";
    fprintf(f,
            "{\"seed\": %llu, \"defects\": %d, \"add\": {\"nibble_a\": %u, \"nibble_b\": %u, \"bit\": %u}, "
            "\"backdoor\": {\"funct3\": %u, \"funct7\": %u, \"magic\": %u}, "
            "\"decode\": {\"variant\": \"%s\", \"mnemonic\": \"%s\", \"param\": %u}}\n",
            (unsigned long long)(PSP_SEED), PSP_DEFECTS, na, nb, fb, sel & 7, sel >> 3, key, vname[cw[5]], mn,
            cw[6]);
}
#endif
