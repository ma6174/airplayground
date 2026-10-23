/*
 * FairPlay's modified MD5 compression (port of fairplay_md5.go).
 *
 * Only the compression function is needed: none of descriptor, masks,
 * digest32 or the wrapping-key KDF ever runs the usual init/update/final
 * sequence, every caller feeds exactly one 64-byte block per call.
 *
 * Two deviations from standard MD5 make mbedtls_md5 unusable:
 *
 *   1. message words are read big endian, while the state is seeded from
 *      little-endian words and written out big endian;
 *   2. right after round 31 the message schedule itself is mutated
 *      (swap / cycle / kdf), using the a,b,c,d values the round just produced.
 *
 * The round function, the constant table and the shift table are plain MD5.
 */
#include <string.h>

#include "fp_internal.h"

#include "fp_data_md5.inc"

/* Reorders the message schedule in place. The three variants are not
 * interchangeable and "swap" in particular is order sensitive: a later swap may
 * move a word an earlier swap just moved, so the loop cannot be vectorised or
 * turned into a permutation applied to indices only. */
static void mutate_message(uint32_t message[16], uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                           fp_mutation mutation)
{
    int index[8];
    index[0] = (int)(a & 15u);
    index[1] = (int)(b & 15u);
    index[2] = (int)(c & 15u);
    index[3] = (int)(d & 15u);
    index[4] = (int)((a >> 4) & 15u);
    index[5] = (int)((b >> 4) & 15u);
    index[6] = (int)((c >> 4) & 15u);
    index[7] = (int)((d >> 4) & 15u);

    switch (mutation) {
    case FP_MUTATION_SWAP:
        for (int i = 0; i < 8; i++) {
            uint32_t tmp = message[i];
            message[i] = message[index[i]];
            message[index[i]] = tmp;
        }
        break;
    case FP_MUTATION_CYCLE: {
        /* Rotating through possibly repeated indices: keep the write order. */
        uint32_t first = message[index[0]];
        for (int i = 0; i < 7; i++) {
            message[index[i]] = message[index[i + 1]];
        }
        message[index[7]] = first;
        break;
    }
    case FP_MUTATION_KDF:
        {
            uint32_t tmp = message[a & 15u];
            message[a & 15u] = message[b & 15u];
            message[b & 15u] = tmp;
            tmp = message[c & 15u];
            message[c & 15u] = message[d & 15u];
            message[d & 15u] = tmp;
            for (int shift = 4; shift <= 12; shift += 4) {
                int i = (int)((a >> shift) & 15u);
                int j = (int)((b >> shift) & 15u);
                tmp = message[i];
                message[i] = message[j];
                message[j] = tmp;
            }
        }
        break;
    }
}

void fp_md5_compress(uint32_t state[4], const uint8_t block[64], fp_mutation mutation)
{
    uint32_t message[16];
    for (int i = 0; i < 16; i++) {
        message[i] = fp_rd_be32(block + i * 4); /* big endian, unlike standard MD5 */
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];

    for (int round = 0; round < 64; round++) {
        uint32_t f;
        int word;
        if (round < 16) {
            f = (b & c) | (~b & d);
            word = round;
        } else if (round < 32) {
            f = (d & b) | (~d & c);
            word = (5 * round + 1) & 15;
        } else if (round < 48) {
            f = b ^ c ^ d;
            word = (3 * round + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            word = (7 * round) & 15;
        }

        uint32_t x = a + f + fairplay_md5_constant[round] + message[word];
        int shift = fairplay_md5_shift[round];
        uint32_t rotated = (x << shift) | (x >> (32 - shift));
        /* Go: a, b, c, d = d, b+rotl(..), b, c - the four assignments read the
         * old values, so the new b must be computed before b is overwritten. */
        uint32_t new_b = b + rotated;
        a = d;
        d = c;
        c = b;
        b = new_b;

        /* Go mutates at the end of round 31 with the post-update registers (note
         * that "a" here is the previous d). Moving this one round earlier or
         * later changes every digest. */
        if (round == 31) {
            mutate_message(message, a, b, c, d, mutation);
        }
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}
