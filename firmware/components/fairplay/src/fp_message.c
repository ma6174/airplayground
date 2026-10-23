/*
 * FairPlay's SAS-CBC message cipher (port of fairplay_message.go).
 *
 * It is an AES-128 round structure driven by externally supplied round keys:
 * one fixed round key before the rounds, nine per mode in the middle and one
 * fixed round key at the end. There is no key schedule to run, which is why
 * neither mbedtls_aes_setkey_enc nor PSA's AES can be used for it: the middle
 * round keys of the four modes are constants from the protocol, not derived
 * from any 16-byte key.
 *
 * Differences from standard AES-128-CBC:
 *
 *   - exactly 128 bytes of data, eight blocks, no padding;
 *   - one fixed IV per mode instead of a caller-supplied IV;
 *   - mode 3 walks the CBC chain backwards (equivalent for separate in/out
 *     buffers, and required for the reference's in-place use - this component
 *     always uses separate buffers).
 *
 * The state layout is column major, state[4 * column + row], matching the Go
 * shift-rows indices below.
 */
#include <string.h>

#include "fp_internal.h"

#include "fp_data_message.inc"

static void add_round_key(uint8_t state[16], const uint8_t key[16])
{
    for (int i = 0; i < 16; i++) {
        state[i] ^= key[i];
    }
}

static void sub_bytes(uint8_t state[16], const uint8_t substitution[256])
{
    for (int i = 0; i < 16; i++) {
        state[i] = substitution[state[i]];
    }
}

/* Go: aesShiftRows, state[4*col + row] = previous[4*((col+row)&3) + row]. */
static void shift_rows(uint8_t state[16])
{
    uint8_t previous[16];
    memcpy(previous, state, sizeof previous);
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            state[4 * col + row] = previous[4 * ((col + row) & 3) + row];
        }
    }
}

static void inverse_shift_rows(uint8_t state[16])
{
    uint8_t previous[16];
    memcpy(previous, state, sizeof previous);
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            state[4 * col + row] = previous[4 * ((col - row + 4) & 3) + row];
        }
    }
}

/* GF(2^8) multiplication with the AES polynomial 0x11b. */
static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    uint8_t product = 0;
    while (b) {
        if (b & 1) {
            product ^= a;
        }
        uint8_t high = (uint8_t)(a & 0x80);
        a = (uint8_t)(a << 1);
        if (high) {
            a ^= 0x1b;
        }
        b >>= 1;
    }
    return product;
}

static void mix_columns(uint8_t state[16])
{
    for (int col = 0; col < 4; col++) {
        int o = col * 4;
        uint8_t a = state[o], b = state[o + 1], c = state[o + 2], d = state[o + 3];
        state[o] = (uint8_t)(gf_mul(a, 2) ^ gf_mul(b, 3) ^ c ^ d);
        state[o + 1] = (uint8_t)(a ^ gf_mul(b, 2) ^ gf_mul(c, 3) ^ d);
        state[o + 2] = (uint8_t)(a ^ b ^ gf_mul(c, 2) ^ gf_mul(d, 3));
        state[o + 3] = (uint8_t)(gf_mul(a, 3) ^ b ^ c ^ gf_mul(d, 2));
    }
}

static void inverse_mix_columns(uint8_t state[16])
{
    for (int col = 0; col < 4; col++) {
        int o = col * 4;
        uint8_t a = state[o], b = state[o + 1], c = state[o + 2], d = state[o + 3];
        state[o] = (uint8_t)(gf_mul(a, 14) ^ gf_mul(b, 11) ^ gf_mul(c, 13) ^ gf_mul(d, 9));
        state[o + 1] = (uint8_t)(gf_mul(a, 9) ^ gf_mul(b, 14) ^ gf_mul(c, 11) ^ gf_mul(d, 13));
        state[o + 2] = (uint8_t)(gf_mul(a, 13) ^ gf_mul(b, 9) ^ gf_mul(c, 14) ^ gf_mul(d, 11));
        state[o + 3] = (uint8_t)(gf_mul(a, 11) ^ gf_mul(b, 13) ^ gf_mul(c, 9) ^ gf_mul(d, 14));
    }
}

/* Encrypt block: key0, then nine rounds of (mix, middle key, sbox, shift),
 * then key10. The round order differs from AES (mix columns first), which the
 * Go source spells out and the round keys are chosen to match. */
static void encrypt_block(uint8_t state[16], uint8_t mode)
{
    add_round_key(state, fairplay_round_key0);
    sub_bytes(state, fairplay_forward_sbox);
    shift_rows(state);
    for (int round = 0; round < 9; round++) {
        mix_columns(state);
        add_round_key(state, fairplay_middle_keys[mode][round]);
        sub_bytes(state, fairplay_forward_sbox);
        shift_rows(state);
    }
    add_round_key(state, fairplay_round_key10);
}

static void decrypt_block(uint8_t state[16], uint8_t mode)
{
    add_round_key(state, fairplay_round_key10);
    for (int round = 9; round > 0; round--) {
        inverse_shift_rows(state);
        sub_bytes(state, fairplay_inverse_sbox);
        add_round_key(state, fairplay_middle_keys[mode][round - 1]);
        inverse_mix_columns(state);
    }
    inverse_shift_rows(state);
    sub_bytes(state, fairplay_inverse_sbox);
    add_round_key(state, fairplay_round_key0);
}

int fp_message_encrypt(uint8_t mode, const uint8_t plain[FP_BODY_LEN], uint8_t out[FP_BODY_LEN])
{
    if (plain == NULL || out == NULL) {
        return FP_ERR_ARG;
    }
    if (mode >= FP_MODE_COUNT) {
        return FP_ERR_MODE;
    }
    const uint8_t *chain = fairplay_message_iv[mode];
    for (int block = 0; block < FP_BODY_LEN / 16; block++) {
        uint8_t state[16];
        for (int i = 0; i < 16; i++) {
            state[i] = (uint8_t)(plain[block * 16 + i] ^ chain[i]);
        }
        encrypt_block(state, mode);
        memcpy(out + block * 16, state, 16);
        chain = out + block * 16;
    }
    return FP_OK;
}

int fp_message_decrypt(uint8_t mode, const uint8_t cipher[FP_BODY_LEN], uint8_t out[FP_BODY_LEN])
{
    if (cipher == NULL || out == NULL) {
        return FP_ERR_ARG;
    }
    if (mode >= FP_MODE_COUNT) {
        return FP_ERR_MODE;
    }
    for (int step = 0; step < FP_BODY_LEN / 16; step++) {
        /* Mode 3 traverses the chain from the last block to the first. With
         * separate buffers this produces the same plaintext as a forward walk;
         * it is kept because it is what the protocol's reference does. */
        int block = (mode == 3) ? (FP_BODY_LEN / 16 - 1 - step) : step;
        uint8_t state[16];
        memcpy(state, cipher + block * 16, 16);
        decrypt_block(state, mode);
        const uint8_t *chain = (block > 0) ? cipher + (block - 1) * 16 : fairplay_message_iv[mode];
        for (int i = 0; i < 16; i++) {
            out[block * 16 + i] = (uint8_t)(state[i] ^ chain[i]);
        }
    }
    return FP_OK;
}

/* Standard AES-128 (normal key schedule) built from the round primitives above.
 * Only ekey[56:72] needs it, so there is no reason to pull in an AES
 * implementation from outside the component. */
void fp_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t round_keys[11][16];
    uint8_t state[16];

    memcpy(round_keys[0], key, 16);
    uint8_t rcon = 0x01;
    for (int round = 1; round <= 10; round++) {
        uint8_t word[4];
        memcpy(word, round_keys[round - 1] + 12, 4);
        /* RotWord, SubWord, Rcon. The Rcon sequence is generated by repeated
         * xtime instead of a table. */
        uint8_t first = word[0];
        word[0] = (uint8_t)(fairplay_forward_sbox[word[1]] ^ rcon);
        word[1] = fairplay_forward_sbox[word[2]];
        word[2] = fairplay_forward_sbox[word[3]];
        word[3] = fairplay_forward_sbox[first];
        rcon = (uint8_t)((rcon << 1) ^ ((rcon & 0x80) ? 0x1b : 0x00));
        for (int i = 0; i < 4; i++) {
            round_keys[round][i] = (uint8_t)(round_keys[round - 1][i] ^ word[i]);
        }
        for (int i = 4; i < 16; i++) {
            round_keys[round][i] = (uint8_t)(round_keys[round - 1][i] ^ round_keys[round][i - 4]);
        }
    }

    memcpy(state, in, 16);
    add_round_key(state, round_keys[0]);
    for (int round = 1; round <= 9; round++) {
        sub_bytes(state, fairplay_forward_sbox);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, round_keys[round]);
    }
    sub_bytes(state, fairplay_forward_sbox);
    shift_rows(state);
    add_round_key(state, round_keys[10]);
    memcpy(out, state, 16);
}
