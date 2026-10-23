/*
 * FairPlay's SAP hash (port of fairplay_sap.go).
 *
 * Not a standard hash: no AES, no MD5. Three stages plus a fold:
 *
 *   load     64 input bytes -> 210 work bytes, four-byte groups reversed
 *   scramble 840 rounds of byte mixing over that 210-byte window
 *   circuit  ~100 straight-line statements over hash[20]/matrix[35]/
 *            aux[10]/work[210]
 *   fold     state folded into 16 bytes, then 256 reverse-diffusion steps
 *
 * Porting hazards are marked inline. The recurring theme is that Go and C
 * disagree about operator precedence: Go puts * / % << >> & &^ on one
 * left-associative level, while C binds << >> tighter than &, & tighter than ^
 * and ^ tighter than |. Every binary operation in the circuit therefore carries
 * explicit parentheses, and values Go computes in int are cast to int here too.
 */
#include <string.h>

#include "fp_internal.h"

#include "fp_data_sap.inc"

/* Short names kept from the transcription so a reviewer can diff the Go source
 * and this file statement by statement. */
typedef uint8_t u8;
typedef uint32_t u32;

/* Go's bits.RotateLeft8 returns the value unchanged for count 0. */
static u8 rotl8(u8 v, unsigned count)
{
    count &= 7u;
    return count ? (u8)((v << count) | (v >> (8 - count))) : v;
}

/* Go byte arithmetic is mod 256 at every step; C promotes to int and truncates
 * on assignment. For + - * & | ^ the two agree (mod 256 is a ring
 * homomorphism), so the circuit may rely on implicit truncation. Division,
 * shifts and unary minus are not homomorphic that way and are cast explicitly
 * wherever they appear. */
static u8 majority(u8 a, u8 b, u8 c) { return (u8)(a ^ ((a ^ b) & (a ^ c))); }

/* select_bits(mask, if_set, if_clear): bitwise mux, Go's selectBits. */
static u8 select_bits(u8 mask, u8 if_set, u8 if_clear)
{
    return (u8)(if_clear ^ ((if_set ^ if_clear) & mask));
}

static u8 square(u8 v) { return (u8)(v * v); }
static u8 cube(u8 v) { return (u8)(v * v * v); }

/* Count 0 yields 0, not v (Go: rotateOrZero). Distinct from rotl8 above. */
static u8 rotate_or_zero(u8 v, u8 count) { return count == 0 ? 0 : rotl8(v, count); }

/* Go: wideSeed. The rotated value is assembled in int and only then reduced
 * modulo 21, so it is deliberately not truncated to 8 bits first. */
static u8 wide_seed(u8 in, u8 count)
{
    if (count == 0) {
        return sap_seed[0];
    }
    int r = ((int)in << count) | ((int)in >> (8 - count));
    return sap_seed[r % 21];
}

/* The Go closures h/m/s/ma index hash/matrix/seed through work[..]; hi/si index
 * them directly. Macros keep the exact argument evaluation of the Go code. */
#define H_(i) hash[work[(i)] % 20]
#define S_(i) sap_seed[work[(i)] % 21]
#define M_(i) matrix[work[(i)] % 35]
#define MA_(i) matrix[aux[(i)] % 35]
#define HI_(i) hash[(i) % 20]
#define SI_(i) sap_seed[(i) % 21]

/* Stage 3. Entered with hash/matrix copied from sap_initial_*, aux zeroed and
 * work filled by the scramble pass. Statement order is part of the algorithm:
 * never reorder, merge or hoist.
 *
 * The block is the Go circuit transcribed verbatim. These are the spots where
 * a naive transcription silently diverges; the left column names the variable
 * the statement writes:
 *
 *   expanded_selector  Go: byte(expansion_bits ^ byte(aux[4]&0xca>>1) ^ 75)
 *                      parses as (aux[4] & 0xca) >> 1. Without parentheses C
 *                      reads aux[4] & (0xca >> 1), which corrupts matrix[18]
 *                      and every later statement.
 *   work[143]          Go: 0x12 | selectBits(..) & (..). Go's | is the lowest
 *                      precedence operator, so the OR applies to the masked
 *                      result; the parentheses spell that out.
 *   hash[19]           int arithmetic: 224|(s(92)&27) and m(41) are promoted
 *   aux[2], hash[10],  to int before / 3, (a+b) >> 1 and 3*x / 5. Doing any of
 *   aux[1], work[164]  these in uint8_t truncates before the divide.
 *   matrix[18], work   wide = int(select_bits(..)); (u32)(wide*wide*wide) >> 1
 *                      and (u32)(wide*wide) / 5 - same reason.
 *   work[15],          unary minus on a byte: Go wraps mod 256, and the low
 *   work[140],         three bits match two's complement, so (-x) & 7 is
 *   matrix[8],         correct while 7 - (x & 7) is not (differs when
 *   matrix[22],        x % 8 == 0). work[5] uses (-(u8)(H_(61) / 5)) & 7,
 *   aux[2], hash[7]    i.e. the division happens on the byte first.
 *   all of the above    rotate_or_zero() returns 0 for count 0 and rotl8()
 *                      returns the value; both appear in this block and
 *                      swapping them changes the hash whenever a count is 0.
 *   wide_seed callers   assemble (in << n) | (in >> (8-n)) in int and reduce
 *                      modulo 21 without an intermediate mod-256 truncation.
 */
static void sap_circuit(u8 hash[20], u8 matrix[35], u8 aux[10], u8 work[210])
{
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
}

#undef H_
#undef S_
#undef M_
#undef MA_
#undef HI_
#undef SI_

void fp_sap_hash(const uint8_t block[64], uint8_t out[16])
{
    u8 hash[20];
    u8 matrix[35];
    u8 aux[10];
    u8 work[210];

    memcpy(hash, sap_initial_hash, sizeof hash);
    memcpy(matrix, sap_initial_matrix, sizeof matrix);
    memset(aux, 0, sizeof aux);

    /* Stage 1: reverse each four-byte group and wrap around until 210 bytes
     * are filled, i.e. work[i] = block[(i & 63) ^ 3]. */
    for (int i = 0; i < 210; i++) {
        work[i] = block[(i & 63) ^ 3];
    }

    /* Stage 2: 840 rounds. `i` must be uint32_t: for i < 155 the expression
     * (i - 155u) wraps around 2^32 before the modulo, and 2^32 % 210 == 46, so
     * unsigned wrap-around is not the same as a signed negative modulo. An int
     * loop variable produces a different hash entirely. */
    for (u32 i = 0; i < 840u; i++) {
        u8 x = work[(i - 155u) % 210u];
        u8 y = work[(i - 57u) % 210u];
        u8 z = work[(i - 13u) % 210u];
        u8 w = work[i % 210u];
        work[i % 210u] = (u8)(rotl8(y, 5) + (rotl8(z, 3) ^ w) - rotl8(x, 7));
    }

    sap_circuit(hash, matrix, aux, work);

    /* Fold the state into the 16-byte output.
     *
     * Only 10 of the 16 output bytes are taken from aux (out[0..2] and
     * out[4..10]); out[3] and out[11..15] stay 0 until the two assignments below,
     * so the memset is load bearing. out[3] and out[11] are assignments, not
     * xor updates, and out[10] is xored with aux[3]^133 before the loop. */
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
    /* 256 reverse-diffusion steps. `i` is int on purpose: (i-7) is negative for
     * the first steps, and & 15 on a negative int is defined here because the
     * mask makes the result 0..15 either way. Never rewrite this as % 16. */
    for (int i = 0; i < 256; i++) {
        out[i & 15] ^= (u8)(rotl8(out[(i - 7) & 15], 1) ^ rotl8(out[(i - 5) & 15], 6) ^ rotl8(out[(i - 1) & 15], 5));
    }
}
