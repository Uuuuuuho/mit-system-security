/* sp.c - Part 1: all-pairs shortest paths on psp-lite (STARTER).
 *
 * Floyd-Warshall over a fixed pseudo-random road network.  On a correct CPU
 * (psp-lite --golden) it prints the true distances; on the defective CPU some
 * of the distance additions come out wrong.  Your job (Exercise 1-1): find out
 * WHICH additions, without trusting the CPU's adder to check itself.
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
/* TODO (Exercise 1-1): every candidate path length goes through path_add().
 *  1. Compute the sum a second way that does not use ADD/ADDI on the operands
 *     (hint: a ripple-carry adder from AND, XOR and shifts) and compare it with
 *     the hardware result (psp_hw_add() in psp.h forces a real ADD).
 *  2. For the first few mismatches print exactly
 *        bad_add a=0x%08x b=0x%08x got=0x%08x want=0x%08x
 *     and count them in n_bad.
 *  3. Continue with the CORRECT value so the final distances match --golden.
 * Check build/student/part1/sp.dis to make sure the compiler did not turn your
 * "second way" back into an ADD. */
static uint32_t n_bad;

static uint32_t path_add(uint32_t a, uint32_t b) { return a + b; }
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
