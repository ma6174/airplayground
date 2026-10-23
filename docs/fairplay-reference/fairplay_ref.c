/* fairplay_ref.c - byte-exact C reference for the FairPlay SAP handshake.
 *
 * Transcribed from the Go reference implementation. Uses only fixed-width
 * integer arithmetic; no AES library is required for the SAP primitives.
 * Build:  cc -O2 -o fairplay_ref fairplay_ref.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fairplay_data.h"
#include "sap_corpus.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

/* ------------------------------------------------------------------ */
/* shared helpers                                                      */
/* ------------------------------------------------------------------ */

static u8 forward_sbox[256];

static u8 rotl8(u8 v, unsigned n) { n &= 7; return n ? (u8)((v << n) | (v >> (8 - n))) : v; }

static u8 majority(u8 a, u8 b, u8 c) { return (u8)(a ^ ((a ^ b) & (a ^ c))); }
static u8 select_bits(u8 mask, u8 if_set, u8 if_clear) { return (u8)(if_clear ^ ((if_set ^ if_clear) & mask)); }
static u8 square(u8 v) { return (u8)(v * v); }
static u8 cube(u8 v) { return (u8)(v * v * v); }
/* Go: rotateOrZero returns 0 when count is 0, otherwise a rotate. */
static u8 rotate_or_zero(u8 v, u8 count) { return count == 0 ? 0 : rotl8(v, count); }

/* Go: wideSeed. `in << count` and `in >> (8-count)` are int arithmetic, so the
 * value is NOT truncated to 8 bits before the modulo. Callers pass count 0..7. */
static u8 wide_seed(u8 in, u8 count)
{
    if (count == 0) return sap_seed[0];
    int r = ((int)in << count) | ((int)in >> (8 - count));
    return sap_seed[r % 21];
}

static u32 rd_be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u32 rd_le32(const u8 *p) { return ((u32)p[3] << 24) | ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0]; }
static void wr_be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void wr_le32(u8 *p, u32 v) { p[3] = (u8)(v >> 24); p[2] = (u8)(v >> 16); p[1] = (u8)(v >> 8); p[0] = (u8)v; }
static void wr_le64(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (8 * i)); }

/* ------------------------------------------------------------------ */
/* modified MD5 compression                                            */
/* ------------------------------------------------------------------ */

enum { FP_SWAP = 0, FP_CYCLE = 1, FP_KDF = 2 };

static void fp_mutate(u32 m[16], u32 a, u32 b, u32 c, u32 d, int mutation)
{
    int idx[8];
    idx[0] = a & 15; idx[1] = b & 15; idx[2] = c & 15; idx[3] = d & 15;
    idx[4] = (a >> 4) & 15; idx[5] = (b >> 4) & 15; idx[6] = (c >> 4) & 15; idx[7] = (d >> 4) & 15;
    switch (mutation) {
    case FP_SWAP:
        /* sequential swaps: a later swap can move an already moved word */
        for (int i = 0; i < 8; i++) {
            u32 t = m[i]; m[i] = m[idx[i]]; m[idx[i]] = t;
        }
        break;
    case FP_CYCLE: {
        u32 first = m[idx[0]];
        for (int i = 0; i < 7; i++) m[idx[i]] = m[idx[i + 1]];
        m[idx[7]] = first;
        break;
    }
    case FP_KDF: {
        u32 t;
        t = m[a & 15]; m[a & 15] = m[b & 15]; m[b & 15] = t;
        t = m[c & 15]; m[c & 15] = m[d & 15]; m[d & 15] = t;
        for (int shift = 4; shift <= 12; shift += 4) {
            int i = (a >> shift) & 15, j = (b >> shift) & 15;
            t = m[i]; m[i] = m[j]; m[j] = t;
        }
        break;
    }
    }
}

static void fp_md5_compress(u32 st[4], const u8 block[64], int mutation)
{
    u32 m[16];
    for (int i = 0; i < 16; i++) m[i] = rd_be32(block + i * 4); /* big-endian words */

    u32 a = st[0], b = st[1], c = st[2], d = st[3];
    for (int round = 0; round < 64; round++) {
        u32 f;
        int word;
        if (round < 16)      { f = (b & c) | (~b & d); word = round; }
        else if (round < 32) { f = (d & b) | (~d & c); word = (5 * round + 1) & 15; }
        else if (round < 48) { f = b ^ c ^ d;          word = (3 * round + 5) & 15; }
        else                 { f = c ^ (b | ~d);       word = (7 * round) & 15; }

        u32 x = a + f + fairplay_md5_constant[round] + m[word];
        int s = fairplay_md5_shift[round];
        u32 nb = b + ((x << s) | (x >> (32 - s)));
        a = d; d = c; c = b; b = nb;

        if (round == 31) fp_mutate(m, a, b, c, d, mutation);
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

/* ------------------------------------------------------------------ */
/* SAP hash                                                            */
/* ------------------------------------------------------------------ */

#define H_(i)  hash[work[(i)] % 20]
#define S_(i)  sap_seed[work[(i)] % 21]
#define M_(i)  matrix[work[(i)] % 35]
#define MA_(i) matrix[aux[(i)] % 35]
#define HI_(i) hash[(i) % 20]
#define SI_(i) sap_seed[(i) % 21]

static void fp_sap_hash(const u8 block[64], u8 out[16])
{
    u8 hash[20], matrix[35], aux[10], work[210];
    memcpy(hash, sap_initial_hash, 20);
    memcpy(matrix, sap_initial_matrix, 35);
    memset(aux, 0, sizeof aux);

    /* reversed 4-byte groups, repeated to fill 210 bytes */
    for (int i = 0; i < 210; i++) work[i] = block[(i & 63) ^ 3];

    /* 4 x 210 scramble passes; indices use unsigned wrap-around */
    for (u32 i = 0; i < 840u; i++) {
        u8 x = work[(i - 155u) % 210u];
        u8 y = work[(i - 57u) % 210u];
        u8 z = work[(i - 13u) % 210u];
        u8 w = work[i % 210u];
        work[i % 210u] = (u8)(rotl8(y, 5) + (rotl8(z, 3) ^ w) - rotl8(x, 7));
    }

    /* --- nonlinear circuit, straight-line, order matters --- */
    matrix[12] = (u8)(0x14 + (select_bits(92, work[64], (u8)(work[99] / 3)) & wide_seed(S_(206), 4)));
    work[4] = (u8)(2 * square((u8)(work[99] / 5)));
    work[153] ^= (u8)(square(M_(203)) * work[190]);
    hash[3] = (u8)(0x13 ^ (((S_(205) >> 1) & 0x10)));
    work[33] = (u8)(work[33] - (u8)(S_(36) & (u8)~9));
    aux[5] = (u8)((((M_(67) & (u8)~2) | 1 | ((H_(181) >> 6) & 2) | (hash[3] & 0x10))) - 15);
    matrix[12] = 0x07;
    work[2] = (u8)(work[2] - 64);
    hash[19] = S_(58);
    aux[4] = (u8)(92 - M_(32));
    aux[9] = (u8)(M_(15) + 0x9e);
    work[34] = (u8)(work[34] + (u8)(SI_(aux[9]) / 5));
    hash[19] = (u8)(hash[19] + (u8)(0xe6 ^ ((HI_(aux[9]) >> 1) & 0x66)));
    work[15] ^= (u8)(3 * rotate_or_zero(work[72], (u8)((-S_(190)) & 7)) - 9 * S_(126));
    hash[15] ^= cube(M_(181));
    matrix[4] ^= (u8)(work[202] / 3);
    matrix[1] = (u8)(matrix[1] + cube(majority((u8)(92 - HI_(aux[4])), (u8)~work[105], 0xc6)));
    hash[19] ^= (u8)(((int)(224 | (S_(92) & 27)) * (int)M_(41)) / 3);
    work[140] = (u8)(work[140] + rotate_or_zero(92, (u8)((-work[5]) & 7)));
    matrix[12] = (u8)(matrix[12] + majority((u8)(~work[4] ^ M_(12)), work[182], 192));
    work[36] = (u8)(work[36] + 125);
    work[124] = rotl8(majority(majority(work[138], hash[15], 74), H_(43), 95), 4);
    {
        u8 aux_hash = HI_(aux[9]);
        aux[1] = (u8)(0x4c & (u8)~(aux_hash & (u8)(S_(68) << 1)));
        aux[2] = (u8)(222 - majority((u8)(((int)work[177] + (int)S_(79)) >> 1), (u8)((3 * (int)work[148]) / 5), matrix[1]));
        matrix[16] = (u8)(matrix[16] + (u8)((((MA_(4) & (u8)~0x60) | aux_hash | 8)) - (u8)((rotl8(work[33], 2) | 128))));
        hash[14] ^= MA_(2);
        work[19] = (u8)(work[19] + majority(rotate_or_zero(SI_(H_(201)), (u8)((M_(112) << 1) & 6)),
                                           (u8)((((H_(208) & (u8)~0x7c)) | (H_(164) & 0x7c)) / 5), 37));
        matrix[8] = (u8)(rotate_or_zero(140, (u8)((-square(S_(45))) & 7)) ^ aux[4]);
        work[190] = 56;
        work[53] = (u8)~(((u8)(H_(83) | 204)) / 5);
        hash[13] = (u8)(hash[13] + H_(41));
        hash[10] = (u8)(majority(MA_(4), work[2], aux[2]) / 15);
        aux[3] = (u8)(92 - square((u8)(0x28 | (MA_(1) & (u8)(0x12 | (S_(2) & 4))))));
    }
    {
        u8 seed_bits = SI_(aux[4]);
        matrix[13] ^= seed_bits;
        aux[6] = (u8)(92 + square(majority((u8)(M_(179) - 38), aux[2], 177)));
        u8 expansion_bits = majority((u8)(aux[3] + (aux[4] & 74)), (u8)~seed_bits, 121);
        work[47] ^= (u8)(M_(89) + majority((u8)(expansion_bits ^ 0xa6), aux[4], 4));
        aux[7] = (u8)((u8)(seed_bits / 3) - MA_(9) -
                      (u8)(0x14 | (work[151] & (u8)((aux[4] & 0x88) | 0x62)) | (aux[4] & 0x22)));
        u8 expanded_selector = (u8)(expansion_bits ^ (u8)((aux[4] & 0xca) >> 1) ^ 75);
        aux[9] = (u8)(aux[9] + (u8)(0x80 | (majority(aux[7], work[151], 0x20) & 0x64) |
                                   (seed_bits & 0x44) | (MA_(9) & 0x1b)));
        matrix[33] ^= work[26];
        matrix[30] = (u8)((u8)((aux[9] / 3) - (u8)((aux[4] & (u8)~8) | 0x13)) ^ H_(122));
        work[22] = (u8)((M_(90) & 0x1b) | 0x44);
        {
            int wide = (int)select_bits(71, matrix[expanded_selector % 35], SI_(aux[5]));
            matrix[18] = (u8)(matrix[18] + (u8)((u32)(wide * wide * wide) >> 1));
        }
        matrix[5] = (u8)(matrix[5] - S_(92));
        matrix[18] ^= (u8)(select_bits(aux[3], MA_(3), select_bits(16, M_(183), work[41])) *
                           select_bits(expanded_selector, H_(59), work[17]));
        matrix[22] = (u8)(majority(select_bits((u8)(hash[14] | 28), (u8)((work[7] & 28) | 0x82), H_(93)),
                                   rotate_or_zero(MA_(4), (u8)(rotate_or_zero(work[11], (u8)((-M_(28)) & 7)) & 7)),
                                   matrix[33]) + 74);
        hash[15] = (u8)(hash[15] - majority(majority(aux[3], aux[4], 214), SI_((u8)(H_(39) ^ 217)), aux[6]));
        {
            u8 hash9 = HI_(aux[9]);
            u8 indexed_hash = HI_((u8)((u8)((aux[4] / 3) - (u8)(aux[9] | work[22])) ^ aux[6] ^
                                       (u8)((((M_(57) | hash9) & (u8)(0x52 | (aux[9] & 0x0d))) |
                                             (((u8)((M_(57) & hash9) | aux[9])) & 0x20)))));
            aux[6] = (u8)(square(square(H_(99))) | MA_(9));
            aux[1] = (u8)(aux[1] +
                          (u8)(rotate_or_zero((u8)(H_(151) | S_(202)), (u8)(H_(50) & 7)) +
                               majority(H_(4),
                                        (u8)(((int)select_bits(matrix[16], indexed_hash, M_(138)) +
                                              (int)select_bits(17, work[33], S_(39))) / 5),
                                        147)));
            aux[0] = select_bits((u8)(hash[10] & 7), (u8)(MA_(6) & H_(209)),
                                 select_bits(0x47, rotate_or_zero(S_(127), (u8)(MA_(6) & 7)),
                                             (u8)(SI_(MA_(5)) << 1)));
            u8 selected_square = select_bits(198, square(M_(14)), (u8)(H_(145) ^ aux[0]));
            u8 seed9 = SI_(aux[9]);
            u8 hash3 = HI_(aux[3]);
            matrix[2] = (u8)(matrix[2] + (u8)(((u8)(hash3 << 1) & (u8)((work[25] & 0x96) | (seed9 & 8))) | (seed9 & 0x40)));
            matrix[14] = (u8)(matrix[14] - select_bits(34, work[97], (u8)(MA_(3) & (aux[0] ^ M_(100)))));
            work[23] ^= (u8)(majority(majority(S_(17), hash3, aux[0]), (u8)(work[50] / 3), 0x76) << 1);
            hash[17] = 115;
            hash[13] = (u8)(((majority(HI_(aux[7]), work[10], 82) >> 1) & 0x68) | (H_(39) & 0x17));
            matrix[33] = (u8)(matrix[33] - (u8)(work[113] & 9));
            matrix[28] = (u8)(matrix[28] - (u8)((aux[3] & (u8)~0x20) | ((work[110] >> 1) & 0x20)));
            work[95] = SI_(aux[3]);
            hash[15] = (u8)(majority((u8)(work[95] - 48), (u8)~work[184], 189) &
                            cube(majority(aux[7], SI_(aux[1]), 0xaa)));
            matrix[22] = (u8)(matrix[22] + work[183]);
            aux[4] ^= (u8)(3 * S_(1));
            aux[5] = (u8)(aux[5] + (u8)((u8)(198 * majority(S_(178), MA_(1), 209)) * H_(13) * (u8)(S_(26) >> 1)));
            aux[8] = select_bits(10, MA_(3), MA_(9));
            matrix[18] = (u8)(matrix[18] - select_bits(hash[15], (u8)(aux[5] / 15), cube((u8)(HI_(aux[6]) | 81))));
            aux[1] = (u8)(aux[1] + (u8)((u8)(SI_(HI_(aux[1])) / 3) - H_(160)));
            hash[16] = (u8)(147 - majority(aux[0],
                                           majority(S_(69), work[172], (u8)(aux[2] - selected_square + 77)),
                                           (u8)(0xc2 | (aux[0] & 5))));
            hash[3] = (u8)(hash[3] - wide_seed(majority(S_(155), work[105], 141),
                                              (u8)(majority(S_(168), H_(29), 6) & 7)));
            work[5] = (u8)(rotate_or_zero(0x38, (u8)((-(u8)(H_(61) / 5)) & 7)) ^ (u8)(((u8)~MA_(8)) / 5));
            work[198] = (u8)(work[198] + work[3]);
            {
                int wide = (int)(162 | MA_(9));
                work[164] = (u8)(work[164] + (u8)((u32)(wide * wide) / 5));
            }
            aux[2] = (u8)(majority(rotate_or_zero(139, (u8)((-aux[5]) & 6)), HI_(aux[3]), 12) |
                          select_bits(95, cube(seed9), HI_(aux[7])));
            matrix[12] = (u8)(matrix[12] + (u8)((16 | ((work[103] | 60) & (aux[2] | (work[103] & 32)))) / 3));
            work[143] = (u8)(work[143] -
                             (u8)(0x12 | (select_bits(aux[9], select_bits(matrix[8], work[35], aux[7]), (u8)(aux[8] / 3)) &
                                          (u8)(0x4d | ((work[172] >> 1) & 0x20)))));
            matrix[29] = 162;
            hash[15] = (u8)(hash[15] + majority((u8)(M_(149) ^ square(work[43])),
                                                (u8)(select_bits(95, H_(125), SI_(aux[1])) >> 1), 115));
            aux[9] = (u8)(aux[9] - HI_(aux[7]));
            hash[7] = (u8)(hash[7] - square(rotate_or_zero(MA_(5), (u8)((-M_(17)) * (M_(17) & 1)))));
            matrix[8] = (u8)(matrix[8] + cube(S_(202)) - work[184]);
            hash[16] = (u8)((M_(102) << 1) & 0x84);
            aux[6] ^= (u8)(SI_(aux[7]) >> 1);
            hash[7] = (u8)(hash[7] - (u8)(H_(191) - select_bits(177, SI_(SI_(aux[1])), (u8)(S_(80) << 1))));
            hash[6] = H_(119);
            hash[12] = (u8)((HI_(aux[8]) ^ (u8)(M_(71) + M_(15))) &
                            majority((u8)((work[118] & (u8)~0x2c) | 2), square(HI_(aux[9])), 27));
            {
                u8 digest_index = (u8)(select_bits(0xa9, (u8)(S_(57) * 231), majority(work[32], MA_(1), 23)) / 5);
                u8 seed_sample = SI_(aux[6]);
                aux[5] = (u8)(majority((u8)((seed_sample & 0x1c) | (H_(82) & 0xa2) | (SI_(digest_index) & 0x41)),
                                       majority(cube(HI_(aux[7])), work[82], 92), 192) ^ digest_index);
                matrix[25] ^= (u8)((u8)(2 * HI_(aux[9]) * work[5]) -
                                   (u8)(rotate_or_zero(aux[4], (u8)(seed_sample & 7)) & (u8)(aux[3] + 110)));
            }
        }
    }

    /* --- fold state into the 16-byte output --- */
    memset(out, 0, 16);
    memcpy(out, aux, 3);
    memcpy(out + 4, aux + 3, 7);
    for (int i = 0; i < 16; i++) out[i] = (u8)(out[i] + 0xe1);
    out[3] = 0x3d;
    out[11] = 0x3c;
    out[10] ^= (u8)(aux[3] ^ 133);
    for (int i = 0; i < 210; i++) {
        u8 value = work[i];
        if (i < 35) value ^= matrix[i];
        if (i < 20) value ^= hash[i];
        out[i & 15] ^= value;
    }
    for (int i = 0; i < 256; i++) {
        out[i & 15] ^= (u8)(rotl8(out[(i - 7) & 15], 1) ^ rotl8(out[(i - 5) & 15], 6) ^ rotl8(out[(i - 1) & 15], 5));
    }
}

#undef H_
#undef S_
#undef M_
#undef MA_
#undef HI_
#undef SI_

/* ------------------------------------------------------------------ */
/* FairPlay message block transform (AES-128 rounds, custom round keys) */
/* ------------------------------------------------------------------ */

static void add_round_key(u8 s[16], const u8 k[16]) { for (int i = 0; i < 16; i++) s[i] ^= k[i]; }
static void sub_bytes(u8 s[16], const u8 box[256]) { for (int i = 0; i < 16; i++) s[i] = box[s[i]]; }

static void shift_rows(u8 s[16])
{
    u8 p[16];
    memcpy(p, s, 16);
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            s[4 * col + row] = p[4 * ((col + row) & 3) + row];
}

static void inv_shift_rows(u8 s[16])
{
    u8 p[16];
    memcpy(p, s, 16);
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            s[4 * col + row] = p[4 * ((col - row + 4) & 3) + row];
}

static u8 gf_mul(u8 a, u8 b)
{
    u8 product = 0;
    while (b) {
        if (b & 1) product ^= a;
        u8 high = a & 0x80;
        a <<= 1;
        if (high) a ^= 0x1b;
        b >>= 1;
    }
    return product;
}

static void mix_columns(u8 s[16])
{
    for (int col = 0; col < 4; col++) {
        int o = col * 4;
        u8 a = s[o], b = s[o + 1], c = s[o + 2], d = s[o + 3];
        s[o]     = (u8)(gf_mul(a, 2) ^ gf_mul(b, 3) ^ c ^ d);
        s[o + 1] = (u8)(a ^ gf_mul(b, 2) ^ gf_mul(c, 3) ^ d);
        s[o + 2] = (u8)(a ^ b ^ gf_mul(c, 2) ^ gf_mul(d, 3));
        s[o + 3] = (u8)(gf_mul(a, 3) ^ b ^ c ^ gf_mul(d, 2));
    }
}

static void inv_mix_columns(u8 s[16])
{
    for (int col = 0; col < 4; col++) {
        int o = col * 4;
        u8 a = s[o], b = s[o + 1], c = s[o + 2], d = s[o + 3];
        s[o]     = (u8)(gf_mul(a, 14) ^ gf_mul(b, 11) ^ gf_mul(c, 13) ^ gf_mul(d, 9));
        s[o + 1] = (u8)(gf_mul(a, 9) ^ gf_mul(b, 14) ^ gf_mul(c, 11) ^ gf_mul(d, 13));
        s[o + 2] = (u8)(gf_mul(a, 13) ^ gf_mul(b, 9) ^ gf_mul(c, 14) ^ gf_mul(d, 11));
        s[o + 3] = (u8)(gf_mul(a, 11) ^ gf_mul(b, 13) ^ gf_mul(c, 9) ^ gf_mul(d, 14));
    }
}

static void fp_encrypt_block(u8 s[16], int mode)
{
    add_round_key(s, fairplay_round_key0);
    sub_bytes(s, forward_sbox);
    shift_rows(s);
    for (int round = 0; round < 9; round++) {
        mix_columns(s);
        add_round_key(s, fairplay_middle_keys[mode][round]);
        sub_bytes(s, forward_sbox);
        shift_rows(s);
    }
    add_round_key(s, fairplay_round_key10);
}

static void fp_decrypt_block(u8 s[16], int mode)
{
    add_round_key(s, fairplay_round_key10);
    for (int round = 9; round > 0; round--) {
        inv_shift_rows(s);
        sub_bytes(s, fairplay_inverse_sbox);
        add_round_key(s, fairplay_middle_keys[mode][round - 1]);
        inv_mix_columns(s);
    }
    inv_shift_rows(s);
    sub_bytes(s, fairplay_inverse_sbox);
    add_round_key(s, fairplay_round_key0);
}

static void fp_encrypt_body(int mode, const u8 plain[128], u8 out[128])
{
    const u8 *chain = fairplay_message_iv[mode];
    for (int block = 0; block < 8; block++) {
        u8 s[16];
        for (int i = 0; i < 16; i++) s[i] = (u8)(plain[block * 16 + i] ^ chain[i]);
        fp_encrypt_block(s, mode);
        memcpy(out + block * 16, s, 16);
        chain = out + block * 16;
    }
}

static void fp_decrypt_body(int mode, const u8 cipher[128], u8 out[128])
{
    for (int step = 0; step < 8; step++) {
        int block = (mode == 3) ? 7 - step : step; /* mode 3 walks backwards */
        u8 s[16];
        memcpy(s, cipher + block * 16, 16);
        fp_decrypt_block(s, mode);
        const u8 *chain = (block > 0) ? cipher + (block - 1) * 16 : fairplay_message_iv[mode];
        for (int i = 0; i < 16; i++) out[block * 16 + i] = (u8)(s[i] ^ chain[i]);
    }
}

/* ------------------------------------------------------------------ */
/* white-box networks                                                  */
/* ------------------------------------------------------------------ */

static u8 lk_subst(const struct lookup *l, u8 v) { return (u8)(fpsap_substitution_bases[l->table][v ^ l->in_xor] ^ l->out_xor); }
static u8 lk_mix(const struct lookup *l, u8 v) { return (u8)(fpsap_mix_bases[l->table][v ^ l->in_xor] ^ l->out_xor); }

static void fpsap_mix_bytes(const struct lookup mix[4][4], u8 state[16], const u8 sub[16])
{
    for (int word = 0; word < 4; word++) {
        int off = word * 4;
        for (int ob = 0; ob < 4; ob++) {
            u8 mixed = 0;
            for (int ib = 0; ib < 4; ib++) mixed ^= lk_mix(&mix[ib][ob], sub[off + ib]);
            state[off + ob] = mixed;
        }
    }
}

static void fpsap_first_network(const u8 masks[9][16], u8 out[16])
{
    u8 state[16];
    for (int i = 0; i < 16; i++) state[i] = (u8)(fpsap_fixed_block[i] ^ fpsap_first_input_mask[i]);
    for (int bank = 0; bank < 9; bank++) {
        u8 sub[16];
        for (int o = 0; o < 16; o++) {
            int in = fpsap_first_position_map[o];
            sub[o] = lk_subst(&fpsap_first_round[bank][in], state[in]);
        }
        fpsap_mix_bytes(fpsap_first_mix, state, sub);
        for (int i = 0; i < 16; i++) state[i] ^= masks[bank][i];
    }
    for (int o = 0; o < 16; o++) {
        int in = fpsap_first_position_map[o];
        out[o] = lk_subst(&fpsap_first_final[in], state[in]);
    }
}

static void fpsap_second_network(const u8 in[16], const u8 masks[9][16], u8 out[16])
{
    u8 state[16];
    memcpy(state, in, 16);
    for (int bank = 8; bank >= 0; bank--) {
        u8 sub[16];
        for (int o = 0; o < 16; o++) {
            int idx = fpsap_second_position_map[o];
            sub[o] = (u8)(lk_subst(&fpsap_second_round[bank][o], state[idx]) ^ masks[bank][o]);
        }
        fpsap_mix_bytes(fpsap_second_mix, state, sub);
    }
    for (int o = 0; o < 16; o++) {
        int idx = fpsap_second_position_map[o];
        out[o] = (u8)(lk_subst(&fpsap_second_final[o], state[idx]) ^ fpsap_second_output_mask[o]);
    }
}

static void fpsap_digest32(const u8 left[16], const u8 right[16], u8 out[16])
{
    u8 block[64];
    u32 st[4] = { 0xb9f3dcdc, 0xfbdc740b, 0x60f77f86, 0x51907216 };
    memset(block, 0, sizeof block);
    memcpy(block, left, 16);
    memcpy(block + 16, right, 16);
    block[32] = 0x80;
    wr_le32(block + 56, 0x100);
    fp_md5_compress(st, block, FP_SWAP);
    for (int i = 0; i < 4; i++) wr_be32(out + i * 4, st[i]);
}

static void fpsap_masks(const u8 seed[20], u8 masks[9][16])
{
    static const u8 suffix[15] = {
        0x57, 0xd8, 0xee, 0xcb, 0xde, 0xfb, 0xcf, 0x59,
        0x1c, 0x27, 0xa2, 0xcf, 0xbe, 0xb0, 0x89,
    };
    u32 st0[4] = { 0x1d4a4587, 0x92f39fcc, 0x1d87d836, 0xcdc86697 };
    for (int i = 0; i < 9; i++) {
        u8 block[64];
        u32 st[4];
        memset(block, 0, sizeof block);
        memcpy(block, seed, 20);
        block[20] = (u8)i;
        memcpy(block + 21, suffix, 15);
        block[36] = 0x80;
        wr_le32(block + 56, 0x320);
        memcpy(st, st0, sizeof st);
        fp_md5_compress(st, block, FP_SWAP);
        for (int w = 0; w < 4; w++) wr_be32(masks[i] + w * 4, st[w]);
    }
}

/* 320-byte buffer, protocol prefix || two SAPs || suffix || MD5 padding */
static void fpsap_descriptor(const u8 m3_sap[128], const u8 m2_sap[128], u8 out[20])
{
    u8 padded[320];
    u32 st[4], first_final[4];
    memset(padded, 0, sizeof padded);
    int off = 0;
    memcpy(padded + off, fpsap_descriptor_prefix, 17); off += 17;
    memcpy(padded + off, m3_sap, 128); off += 128;
    memcpy(padded + off, m2_sap, 128); off += 128;
    memcpy(padded + off, fpsap_descriptor_suffix, 17); off += 17;
    padded[off] = 0x80;
    wr_le64(padded + 312, (u64)off * 8); /* off == 290, so 2320 bits */

    for (int w = 0; w < 4; w++) st[w] = rd_le32(fairplay_initial_session_key + w * 4);
    for (int o = 0; o < 320; o += 64) {
        u8 add[16];
        fp_sap_hash(padded + o, add);
        for (int w = 0; w < 4; w++) st[w] += rd_le32(add + w * 4);
        fp_md5_compress(st, padded + o, FP_CYCLE);
        if (o == 256) { /* last block: compress twice */
            memcpy(first_final, st, sizeof st);
            fp_md5_compress(st, padded + o, FP_CYCLE);
        }
    }
    wr_be32(out, first_final[0]);
    for (int w = 0; w < 4; w++) wr_be32(out + 4 + w * 4, st[w]);
}

static void fpsap_exchange_seed(const u8 seed[20], u8 out[20])
{
    u8 masks[9][16], intermediate[16], left[16], white[16], digest[16];
    fpsap_masks(seed, masks);
    fpsap_first_network(masks, intermediate);
    fpsap_digest32(intermediate, fpsap_fixed_block, left);
    fpsap_second_network(left, masks, white);
    fpsap_digest32(left, white, digest);
    memcpy(out, white, 4);
    memcpy(out + 4, digest, 16);
}

static void fpsap_exchange_for_sap(const u8 m3_sap[128], const u8 m2_sap[128], u8 out[20])
{
    u8 seed[20];
    fpsap_descriptor(m3_sap, m2_sap, seed);
    fpsap_exchange_seed(seed, out);
}

/* ------------------------------------------------------------------ */
/* AES-128 (standard, for the ekey wrapping block only)                */
/* ------------------------------------------------------------------ */

static void aes128_expand(const u8 key[16], u8 rk[176])
{
    static const u8 rcon[10] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36 };
    memcpy(rk, key, 16);
    for (int i = 4; i < 44; i++) {
        u8 t[4];
        memcpy(t, rk + (i - 1) * 4, 4);
        if (i % 4 == 0) {
            u8 first = t[0];
            t[0] = (u8)(forward_sbox[t[1]] ^ rcon[i / 4 - 1]);
            t[1] = forward_sbox[t[2]];
            t[2] = forward_sbox[t[3]];
            t[3] = forward_sbox[first];
        }
        for (int j = 0; j < 4; j++) rk[i * 4 + j] = (u8)(rk[(i - 4) * 4 + j] ^ t[j]);
    }
}

static void aes128_encrypt_block(const u8 rk[176], const u8 in[16], u8 out[16])
{
    u8 s[16];
    memcpy(s, in, 16);
    add_round_key(s, rk);
    for (int round = 1; round <= 9; round++) {
        sub_bytes(s, forward_sbox);
        shift_rows(s);
        mix_columns(s);
        add_round_key(s, rk + round * 16);
    }
    sub_bytes(s, forward_sbox);
    shift_rows(s);
    add_round_key(s, rk + 160);
    memcpy(out, s, 16);
}

/* ------------------------------------------------------------------ */
/* wrapping key and ekey                                               */
/* ------------------------------------------------------------------ */

static void wrapping_key(const u8 receiver_sap[128], const u8 m3[164], u8 out[16])
{
    u8 decrypted[128], material[320];
    u32 st[4];
    int mode = m3[12];
    fp_decrypt_body(mode, m3 + 16, decrypted);

    memset(material, 0, sizeof material);
    int off = 0;
    memcpy(material + off, fairplay_kdf_prefix, 17); off += 17;
    memcpy(material + off, decrypted, 128); off += 128;
    memcpy(material + off, receiver_sap, 128); off += 128;
    memcpy(material + off, fairplay_kdf_suffix, 17); off += 17;
    material[off] = 0x80;
    wr_le64(material + 312, (u64)off * 8); /* off == 290 */

    for (int w = 0; w < 4; w++) st[w] = rd_le32(fairplay_initial_session_key + w * 4);
    for (int o = 0; o < 320; o += 64) {
        u8 hashed[16];
        fp_md5_compress(st, material + o, FP_KDF);
        fp_sap_hash(material + o, hashed);
        for (int w = 0; w < 4; w++) st[w] += rd_le32(hashed + w * 4);
    }
    for (int w = 0; w < 4; w++) wr_be32(out + w * 4, st[w]);
}

/* sender-side ekey record; the HMAC-SHA1 is standard and omitted here */
static void wrap_ekey(const u8 receiver_sap[128], const u8 m3[164], const u8 raw[16],
                      const u8 mask[16], u8 ekey[72])
{
    static const u8 header[16] = {
        'F', 'P', 'L', 'Y', 0x01, 0x02, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00, 0x00,
    };
    u8 key[16], rk[176], masked[16], block[16];
    memcpy(ekey, header, 16);
    memcpy(ekey + 16, mask, 16);
    wr_be32(ekey + 32, 16);
    memset(ekey + 36, 0, 20); /* HMAC-SHA1 slot, filled by mbedtls in production */
    wrapping_key(receiver_sap, m3, key);
    aes128_expand(key, rk);
    for (int i = 0; i < 16; i++) masked[i] = (u8)(raw[i] ^ mask[i]);
    aes128_encrypt_block(rk, masked, block);
    memcpy(ekey + 56, block, 16);
}

/* ------------------------------------------------------------------ */
/* tests                                                               */
/* ------------------------------------------------------------------ */

static void hexdec(const char *hex, u8 *out, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (u8)v;
    }
}

static void hexprint(const u8 *b, int len)
{
    for (int i = 0; i < len; i++) printf("%02x", b[i]);
}

static int failures;

static void expect_hex(const char *name, const u8 *got, int len, const char *want)
{
    u8 w[256];
    hexdec(want, w, len);
    if (memcmp(got, w, len) != 0) {
        printf("FAIL %s\n  got  ", name); hexprint(got, len); printf("\n  want %s\n", want);
        failures++;
    } else {
        printf("ok   %s\n", name);
    }
}

static const u8 reference_local_sap[128] = {
    /* filled in by hexdec at startup */
};

int main(void)
{
    for (int e = 0; e < 256; e++) forward_sbox[fairplay_inverse_sbox[e]] = (u8)e;

    static u8 local_sap[128], receiver_sap[128];
    hexdec("0001e4e3dd688293e6fa66b95ba41768e587c65f750218ff1be21543d573cefb"
           "087bd36e0c6363c3c8242f4abcfa6d660b801032015405eb4ab04dda7aeff38f"
           "fb36f4cfa48f0b5d92ae363f68b45925bbe6413ab6bdc4968f548d21e67d20f"
           "1912b6820e53f1013cde29df7350a9b9fa7c51320aea62d2949786c87642e34ba", local_sap, 128);
    hexdec("0001cc342a5e5b1a6773c20e21b8224df862481864ef810aae2e3703c8819c23"
           "539de5f5d749bc5b7a266c496283ce7f03937ae1f616de0c15ff338ccaffb09e"
           "aabbe40f5d5f558fb97f1731f8f7da60a0ec6579c33ea98312c3b67135a6694f"
           "f82305d9ba5c615fa254d2b1834583cee42d4426c835a7a5f6c8421c0da3f1c7", receiver_sap, 128);
    (void)reference_local_sap;

    /* 1. SAP hash corpus + single vector */
    for (int k = 0; k < 64; k++) {
        u8 block[64], digest[16], want[16];
        char name[32];
        hexdec(sap_corpus[k][0], block, 64);
        hexdec(sap_corpus[k][1], want, 16);
        fp_sap_hash(block, digest);
        snprintf(name, sizeof name, "sap_hash[%d]", k);
        expect_hex(name, digest, 16, sap_corpus[k][1]);
    }
    {
        u8 block[64], digest[16];
        for (int i = 0; i < 64; i++) block[i] = (u8)(i * 3 + 1);
        fp_sap_hash(block, digest);
        expect_hex("sap_hash(i*3+1)", digest, 16, "75498a4e218773030e9cdf04f0c49367");
    }

    /* 2. modified MD5 */
    {
        u8 block[64], key[16], words[16];
        u32 st[4];
        for (int i = 0; i < 64; i++) block[i] = (u8)(i * 3 + 1);
        for (int i = 0; i < 16; i++) key[i] = (u8)i;
        const char *want[3] = { "e5c410068489d23a0b9ed242c0f0a487", /* swap */
                                "290d7c25132dd69555d47bca3156b0ce", /* cycle */
                                "cb28f7f6b697435a924f6675aa59b891" }; /* kdf */
        for (int mut = 0; mut < 3; mut++) {
            for (int w = 0; w < 4; w++) st[w] = rd_le32(key + w * 4);
            fp_md5_compress(st, block, mut);
            for (int w = 0; w < 4; w++) wr_be32(words + w * 4, st[w]);
            char name[32];
            snprintf(name, sizeof name, "md5_mutation[%d]", mut);
            expect_hex(name, words, 16, want[mut]);
        }
    }

    /* 3. message block transform, 4 modes */
    {
        const char *plain[4] = {
            "b66a3295ffa6b56e02ed1b3d67fef74b90fe148570de65e6773669126a4905d8405644cae0b2f5ed6109c099c7aea7398dac8d623fbd69b87242b374d98f89502bb5a63e29c46a8ed0e98466966191ec1e6c8675087fde21337db1c8fab4c21db824026335f6fc37e2e5b6f53357d06994bd383d6029a0aff654fb1521bcdde4",
            "0f95c6ddc8987eda18577da2db074e7c04715af8b3914a73be1b3d6c111953017ee0a39dfcab3e0d57f2f9fbd59c5e18101788c2ab8e3cbb403bcb48b53f3e5bf74f949e79fa5ca679df4bfcb33a69b1442675d03f948fe5bd0c5ffb64b73a5ab58f46d6baae097b599624147c2487991163ecffc4d966240f9526346a10fdb0",
            "40f18751b44d733e0aa0416401a7d3f40375fad3ce56900602578bca14660909820e6ef3a5e943cafef5370f72c52177d9b82278b414811201a3d99202bedcca26a4d1ad08bc2669f4bae6ca54b8a120d0425edb6082f51f5aecdb547bfdb319099c9ea2729ae6a1c4480827ce9991e273843cf1c7d74ebbebc2657659bcea9f",
            "70a3c30edf0e1dfa1785ce4336ed547062672a47f714a0c1f89a83d95691103dfe5cf653d4cb8299793faf33fd0d4482ef5333b41ab094a90e1baf996bcf4989783f6918397fbacddaf00a2b97556dd8099841578bc5eb1444912b47298eaf356fdd6701bb3f64e725a80eb4c6f3556195de35c93e7cc703bdd24351468e9847",
        };
        for (int mode = 0; mode < 4; mode++) {
            u8 cipher[128], want_cipher[128], plain_out[128], encrypted[128];
            for (int i = 0; i < 128; i++) cipher[i] = (u8)((i + 16) * 5 + 7);
            fp_decrypt_body(mode, cipher, plain_out);
            char name[32];
            snprintf(name, sizeof name, "message_plain[%d]", mode);
            expect_hex(name, plain_out, 128, plain[mode]);
            fp_encrypt_body(mode, plain_out, encrypted);
            memcpy(want_cipher, cipher, 128);
            if (memcmp(encrypted, want_cipher, 128) != 0) {
                printf("FAIL message_cipher[%d] round-trip\n", mode);
                failures++;
            } else {
                printf("ok   message_cipher[%d]\n", mode);
            }
        }
    }

    /* 4. descriptor + exchange vectors */
    {
        struct { const char *name; const char *m2_sap; const char *descriptor; const char *exchange; } v[] = {
            { "all-zeros", "00456e82e18efa5798ba98d6f71060ad2742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a0",
              "7e38958ffe4ed433743919fe7eb16376afa4eb9e", "6f627565f3e77f5b5ede91beee7baf92e4241e0b" },
            { "all-ff", "34c4dc74e4e1e7c0e4282a0a271c041fec3ca8aa38280645c74adee48bb792edec3ca8aa38280645c74adee48bb792edec3ca8aa38280645c74adee48bb792edec3ca8aa38280645c74adee48bb792edec3ca8aa38280645c74adee48bb792edec3ca8aa38280645c74adee48bb792edec3ca8aa38280645c74adee48bb792ed",
              "0e521732cc8f7712c4330b73dc6168078af62d47", "dc2cc74f2ed55484f59f95b96082f0f5c017dd17" },
            { "captured-m2", "00018b2d9ac45a1d0134ccc1d9f09168da5298d19f066ca3355634076b7ff73635bae69475e68ccc45e9800df9e3f4ebe38d9068d3488cfde5140d146ad834d87d81b55e54989e5b5b329bfc9be1807ef1ab9107e91cf8725d6b2af6dd5ab02e76b28cbc05cd82c950ffd5f213d52ff45dee7e842b897d43face99c853ead538",
              "88f54b0f07b660c8a81b1029e777fccce590540e", "4b911e48af23d8406368aeafbb61bfcd569e3e55" },
            { "42-at-0", "bec2a74439b984a5000fa240b9a4d15e6542e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a0",
              "638cf32c301873bea451169f6bd3a3e8c96cf875", "9bfb9556b8659c2ac94b7ef9e587d71e159ea624" },
            { "42-at-63", "00456e82e18efa5798ba98d6f71060ad2742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a00d4c7110dc712e93ffd3b47ab9a2e8342742e5a3c2b8e42d442793c7a44409e22742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a0",
              "1de544d6a75911d3836071200306f49268d6559a", "150d9fa4eb456e73ba48de5779c5c996b16b3b23" },
            { "42-at-64", "00456e82e18efa5798ba98d6f71060ad2742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a099c52c651a8f9adfdc92a951eaf0b8536542e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a0",
              "1c74ef083db6ad37300d4b1aab7facdc9ea4b956", "a167db30424ff8890d085c0f1c92b2c5cc06fc45" },
            { "42-at-127", "00456e82e18efa5798ba98d6f71060ad2742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a02742e5a3c2b8e42d442793c7a44409a00d4c7110dc712e93ffd3b47ab9a2e834",
              "e3bac81ee204258c1248ee8b23669615c5ac7120", "d246ec5e7adc8118994b8df77146529486ac7caf" },
        };
        for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
            u8 m2_sap[128], seed[20], exchange[20];
            hexdec(v[i].m2_sap, m2_sap, 128);
            fpsap_descriptor(local_sap, m2_sap, seed);
            char name[64];
            snprintf(name, sizeof name, "%s descriptor", v[i].name);
            expect_hex(name, seed, 20, v[i].descriptor);
            fpsap_exchange_for_sap(local_sap, m2_sap, exchange);
            snprintf(name, sizeof name, "%s exchange20", v[i].name);
            expect_hex(name, exchange, 20, v[i].exchange);
        }
    }

    /* 5. full m3 + ekey chain against the mode-3 receiver SAP */
    {
        u8 m2[142], m3[164], expected[164];
        hexdec("46504c59030102000000008202039001e1727e0f57f9f5880db104a6257a23f5cfff1abbe1e93045251afb97eb9fc0011ebe0f3a81df5b691d76acb2f7a5c708e3d328f56bb39dbde5f29c8a17f481487e3ae863c678325422e6f78e166d18aa7fd636258bce28726f661f738893ce44311e4be6c0535193e5ef72e8686233729c227d820c999445d89246c8c359", m2, 142);
        hexdec("46504c590301030000000098038f1a9c7ddd506f27dfcaf90b08b73940966603a44b2ddb4df5ea46b9846ae655124558231ace6d9cc1419392a7d2e7852139b94301f227ab0427b748961c0fb9e2a27a8929618638e29a99a6d5743bb03273f5f98932495b3abfad59110341f892728cee7447a7e3eea1e7a29825da08be9dd4cfe45b5a3e925ad6d4169a217f7fb0672ff6f0d976a8edafa9013d99cc3efe6ee0bd522f", expected, 164);

        int mode = m2[13];
        u8 m2_sap[128], tail[20], body[128], session_sap[128];
        session_sap[0] = 0x00;
        session_sap[1] = 0x01;
        for (int i = 2; i < 128; i++) session_sap[i] = (u8)(i - 1);
        fp_decrypt_body(mode, m2 + 14, m2_sap);
        memcpy(m3, "FPLY", 4);
        m3[4] = 3; m3[5] = 1; m3[6] = 3; m3[7] = 0;
        wr_be32(m3 + 8, 152);
        m3[12] = (u8)mode;
        memcpy(m3 + 13, fpsap_m3_label, 3);
        fp_encrypt_body(mode, session_sap, body);
        memcpy(m3 + 16, body, 128);
        fpsap_exchange_for_sap(session_sap, m2_sap, tail);
        memcpy(m3 + 144, tail, 20);
        u8 want[164];
        hexdec("46504c590301030000000098038f1a9c7ddd506f27dfcaf90b08b73940966603a44b2ddb4df5ea46b9846ae655124558231ace6d9cc1419392a7d2e7852139b94301f227ab0427b748961c0fb9e2a27a8929618638e29a99a6d5743bb03273f5f98932495b3abfad59110341f892728cee7447a7e3eea1e7a29825da08be9dd4cfe45b5a3e925ad6d4169a217f7fb0672ff6f0d976a8edafa9013d99cc3efe6ee0bd522f", want, 164);
        if (memcmp(m3, want, 164) != 0) { printf("FAIL m3 bytes\n"); failures++; }
        else printf("ok   m3 bytes\n");
        (void)expected;

        u8 raw[16], mask[16], ekey[72], want_ekey[72];
        for (int i = 0; i < 16; i++) raw[i] = (u8)i;
        memset(mask, 0x5a, 16);
        wrap_ekey(receiver_sap, m3, raw, mask, ekey);
        hexdec("46504c59010201000000003c000000005a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a00000010806e886eba85f78947fe5cd52d8af2f0873833b39a11b854e4a681872bf5f46226c182fc", want_ekey, 72);
        if (memcmp(ekey, want_ekey, 36) != 0) { printf("FAIL ekey[0:36]\n"); failures++; }
        else printf("ok   ekey[0:36]\n");
        if (memcmp(ekey + 56, want_ekey + 56, 16) != 0) {
            printf("FAIL ekey[56:72]\n  got  "); hexprint(ekey + 56, 16); printf("\n");
            failures++;
        } else printf("ok   ekey[56:72]\n");
    }

    /* 6. wrapping key vectors from the message tests */
    {
        const char *keys[4] = { "f7dd1ccb9e745f7951a6e325d73a1f5f", "b44ad891396f097aa309bc132f5b8889",
                                "d38cd8efecdb20f333273c4312d9b236", "769e2fe4c5ad7fbe6fd6772d00f529f4" };
        for (int mode = 0; mode < 4; mode++) {
            u8 m3[164], key[16];
            memset(m3, 0, sizeof m3);
            m3[12] = (u8)mode;
            for (int i = 16; i < 144; i++) m3[i] = (u8)(i * 5 + 7);
            wrapping_key(receiver_sap, m3, key);
            char name[32];
            snprintf(name, sizeof name, "wrapping_key[%d]", mode);
            expect_hex(name, key, 16, keys[mode]);
        }
    }

    /* 7. upstream captured unwrap vector */
    {
        u8 m3[164], ekey[72], sender_sap[128], mac_key[20], key[16];
        hexdec("46504c590301030000000098018f1a9c7d0af257b31f21f5c2d2bc814c032d457835ad0b06250574bbc7ab4a58cca6eead2c911d7f3e1e7ed4c058955dff3d5ceef014387a985bdb34995015e3dfbdacc56047cb926e093b13e9fdb5e1eee317c018bbc87fc5453c7671647da686da3d564875d03f8aea9d60092de06110bc7be0c16f391c369c75344ae47f33acfcf10e63a9b58bfce215e96001c49e4be967c5067f2a", m3, 164);
        hexdec("46504c59010201000000003c0000000088e4f82c8178c18b4751ac24b27c0c2a00000010c899dc6965c1081de6a9d966e2ba3e34548cdbc651c322db18dc22f58fe154a60aecee18", ekey, 72);
        wrapping_key(receiver_sap, m3, key);
        expect_hex("unwrap wrapping key", key, 16, "bc94d7985601318b0ed5d9d33af125e8");
        fp_decrypt_body(m3[12], m3 + 16, sender_sap);
        expect_hex("unwrap sender sap", sender_sap, 128, "0001e023dda8b80379504c3c08b55451ff3cd390979119802841b03c1cc4358958db49387a3c8ec2e4c60b29fd493814889795b54ee2e1a3398e0e90b47bd164d29e98b2d61d58c372a67b23698fbfcfa51741e5a146e06d42907cd89832e4dcafb001847262ac6c5ac220f676b3cf801e7673b954c3f223f94f6117a1370ca5");
        fpsap_descriptor(sender_sap, receiver_sap, mac_key);
        expect_hex("unwrap mac key", mac_key, 20, "291c49a06396bd0efe996926c7ca5cf52dc2d2cf");
    }

    printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
