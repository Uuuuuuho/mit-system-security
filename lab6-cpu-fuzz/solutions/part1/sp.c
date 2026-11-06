/* sp.c - Part 1: all-pairs shortest paths on psp-lite (REFERENCE SOLUTION - spoiler).
 *
 * Floyd-Warshall over a fixed pseudo-random road network.  On a correct CPU
 * (psp-lite --golden) it prints the true distances; on the defective CPU some
 * of the distance additions come out wrong.  checked_add() recomputes every
 * addition with an adder built from AND/XOR/shift, reports the first operands
 * the hardware got wrong, and continues with the correct value.
 */
#include "psp.h"

#define N 32
#define INF 0x3FFFFFFFu

static uint32_t dist[N][N];
static uint32_t rng_state;

/* xorshift32: shifts and xors only, so the graph itself never touches the adder */
static uint32_t rng(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

static void build_graph(void) {
    rng_state = 0x5EED2024u; /* fixed: everybody gets the same graph */
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) dist[i][j] = (i == j) ? 0 : INF;
    for (int i = 0; i < N; i++) {
        for (int e = 0; e < 4; e++) { /* four one-way roads leave every node */
            uint32_t j = rng() % N;
            uint32_t w = (rng() & 0x7FFu) | 0x100u; /* road length 256..2047 */
            if (j != (uint32_t)i && w < dist[i][j]) dist[i][j] = w;
        }
    }
}

/* ---------------------------------------------------------------- Exercise 1-1 */
/* Ripple-carry addition from AND, XOR and shifts: no ADD/ADDI on the operands.
 * (Check build/.../sp.dis: the loop body is and/xor/slli/bne.) */
static uint32_t soft_add(uint32_t a, uint32_t b) {
    while (b) {
        uint32_t carry = a & b;
        a ^= b;
        b = carry << 1;
    }
    return a;
}

static uint32_t n_bad;

static uint32_t checked_add(uint32_t a, uint32_t b) {
    uint32_t hw = psp_hw_add(a, b); /* what the CPU's adder says */
    uint32_t sw = soft_add(a, b);   /* what it should say */
    if (hw != sw) {
        if (n_bad < 4) printf("bad_add a=0x%08x b=0x%08x got=0x%08x want=0x%08x\n", a, b, hw, sw);
        n_bad++;
    }
    return sw;
}

static uint32_t path_add(uint32_t a, uint32_t b) { return checked_add(a, b); }
/* ------------------------------------------------------------------------------ */

/* fingerprint of every candidate length considered (a debugging aid: rotate + xor, no adds) */
static uint32_t fingerprint;

static void floyd_warshall(void) {
    for (int k = 0; k < N; k++)
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) {
                if (dist[i][k] >= INF || dist[k][j] >= INF) continue;
                uint32_t alt = path_add(dist[i][k], dist[k][j]);
                fingerprint = ((fingerprint << 5) | (fingerprint >> 27)) ^ alt;
                if (alt < dist[i][j]) dist[i][j] = alt;
            }
}

int main(void) {
    build_graph();
    floyd_warshall();
    uint32_t check = 0;
    for (int j = 0; j < N; j++) {
        if (dist[0][j] >= INF)
            printf("dist 0->%d = inf\n", j);
        else
            printf("dist 0->%d = %u\n", j, dist[0][j]);
    }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) check ^= dist[i][j] << (j & 7);
    printf("checksum = %08x\n", check);
    printf("fingerprint = %08x\n", fingerprint);
    printf("bad additions detected: %u\n", n_bad);
    return 0;
}
