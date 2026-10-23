/*
 * Test-only backing for fake_psa/psa/crypto.h: HKDF-SHA512 and
 * ChaCha20-Poly1305 (RFC 8439), written from the specifications so the
 * component's PSA call sequence and pairing messages can run on the host.
 *
 * The host tests compare these against vectors from reference.py, which checks
 * itself against RFC 8439 2.8.2 first. Only the device uses real mbedTLS.
 */
#include "psa/crypto.h"

#include <string.h>

#include "sha512.h"

#define KEY_SLOTS 8
#define POLY_MASK 0x3FFFFFFu

typedef struct {
    int in_use;
    uint8_t key[32];
    psa_key_usage_t usage;
} key_slot_t;

static key_slot_t slots[KEY_SLOTS];

void psa_reset_key_attributes(psa_key_attributes_t *attributes)
{
    memset(attributes, 0, sizeof *attributes);
}

psa_status_t psa_crypto_init(void)
{
    return PSA_SUCCESS;
}

/* ------------------------------------------------------------------- HKDF */

psa_status_t psa_key_derivation_setup(psa_key_derivation_operation_t *operation,
                                      psa_algorithm_t algorithm)
{
    memset(operation, 0, sizeof *operation);
    if ((algorithm & 0xFFFFFF00u) != 0x08000000u ||
        (algorithm & 0xFFu) != (PSA_ALG_SHA_512 & 0xFFu)) {
        return PSA_ERROR_NOT_SUPPORTED;
    }
    operation->algorithm = algorithm;
    return PSA_SUCCESS;
}

psa_status_t psa_key_derivation_input_bytes(psa_key_derivation_operation_t *operation,
                                            psa_key_derivation_step_t step, const uint8_t *data,
                                            size_t data_length)
{
    uint8_t *target;
    size_t *target_len;
    size_t capacity;

    switch (step) {
    case PSA_KEY_DERIVATION_INPUT_SALT:
        target = operation->salt;
        target_len = &operation->salt_len;
        capacity = sizeof operation->salt;
        break;
    case PSA_KEY_DERIVATION_INPUT_SECRET:
        target = operation->secret;
        target_len = &operation->secret_len;
        capacity = sizeof operation->secret;
        break;
    case PSA_KEY_DERIVATION_INPUT_INFO:
        target = operation->info;
        target_len = &operation->info_len;
        capacity = sizeof operation->info;
        break;
    default:
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    if (data_length > capacity) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    memcpy(target, data, data_length);
    *target_len = data_length;
    return PSA_SUCCESS;
}

static void hmac_sha512(uint8_t out[64], const uint8_t *key, size_t key_len, const uint8_t *data,
                        size_t data_len)
{
    uint8_t block[128];
    uint8_t inner[64];
    sha512_context hash;
    size_t index;

    memset(block, 0, sizeof block);
    memcpy(block, key, key_len);
    for (index = 0; index < sizeof block; index++) {
        block[index] ^= 0x36;
    }
    sha512_init(&hash);
    sha512_update(&hash, block, sizeof block);
    sha512_update(&hash, data, data_len);
    sha512_final(&hash, inner);

    for (index = 0; index < sizeof block; index++) {
        block[index] ^= 0x36 ^ 0x5c;
    }
    sha512_init(&hash);
    sha512_update(&hash, block, sizeof block);
    sha512_update(&hash, inner, sizeof inner);
    sha512_final(&hash, out);
}

psa_status_t psa_key_derivation_output_bytes(psa_key_derivation_operation_t *operation,
                                             uint8_t *output, size_t output_length)
{
    uint8_t prk[64];
    uint8_t block[64];
    uint8_t buffer[64 + 64 + 1];
    size_t produced = 0;
    size_t block_len = 0;
    uint8_t counter = 1;

    if (operation->secret_len == 0) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    hmac_sha512(prk, operation->salt, operation->salt_len, operation->secret,
                operation->secret_len);

    while (produced < output_length) {
        size_t take = output_length - produced;

        memcpy(buffer, block, block_len);
        memcpy(buffer + block_len, operation->info, operation->info_len);
        buffer[block_len + operation->info_len] = counter;
        hmac_sha512(block, prk, sizeof prk, buffer, block_len + operation->info_len + 1);
        block_len = sizeof block;
        counter++;

        if (take > sizeof block) {
            take = sizeof block;
        }
        memcpy(output + produced, block, take);
        produced += take;
    }
    return PSA_SUCCESS;
}

psa_status_t psa_key_derivation_abort(psa_key_derivation_operation_t *operation)
{
    memset(operation, 0, sizeof *operation);
    return PSA_SUCCESS;
}

/* ------------------------------------------------------------- key slots */

psa_status_t psa_import_key(const psa_key_attributes_t *attributes, const uint8_t *data,
                            size_t data_length, mbedtls_svc_key_id_t *key)
{
    size_t index;

    if (attributes->type != PSA_KEY_TYPE_CHACHA20 || data_length != 32) {
        return PSA_ERROR_NOT_SUPPORTED;
    }
    for (index = 0; index < KEY_SLOTS; index++) {
        if (!slots[index].in_use) {
            slots[index].in_use = 1;
            memcpy(slots[index].key, data, data_length);
            slots[index].usage = attributes->usage;
            *key = (mbedtls_svc_key_id_t)(index + 1);
            return PSA_SUCCESS;
        }
    }
    return PSA_ERROR_INSUFFICIENT_STORAGE;
}

psa_status_t psa_destroy_key(mbedtls_svc_key_id_t key)
{
    if (key == 0 || key > KEY_SLOTS || !slots[key - 1].in_use) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    memset(&slots[key - 1], 0, sizeof slots[key - 1]);
    return PSA_SUCCESS;
}

/* -------------------------------------------------------- ChaCha20 */

static uint32_t rotl32(uint32_t value, int count)
{
    return (value << count) | (value >> (32 - count));
}

static uint32_t load32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void chacha20_block(uint8_t out[64], const uint8_t key[32], uint32_t counter,
                           const uint8_t nonce[12])
{
    static const uint8_t constants[16] = "expand 32-byte k";
    uint32_t state[16];
    uint32_t work[16];
    int round;
    int index;

    for (index = 0; index < 4; index++) {
        state[index] = load32(constants + 4 * index);
    }
    for (index = 0; index < 8; index++) {
        state[4 + index] = load32(key + 4 * index);
    }
    state[12] = counter;
    for (index = 0; index < 3; index++) {
        state[13 + index] = load32(nonce + 4 * index);
    }
    memcpy(work, state, sizeof work);

#define QUARTER(a, b, c, d)                     \
    work[a] += work[b];                         \
    work[d] = rotl32(work[d] ^ work[a], 16);    \
    work[c] += work[d];                         \
    work[b] = rotl32(work[b] ^ work[c], 12);    \
    work[a] += work[b];                         \
    work[d] = rotl32(work[d] ^ work[a], 8);     \
    work[c] += work[d];                         \
    work[b] = rotl32(work[b] ^ work[c], 7);

    for (round = 0; round < 10; round++) {
        QUARTER(0, 4, 8, 12)
        QUARTER(1, 5, 9, 13)
        QUARTER(2, 6, 10, 14)
        QUARTER(3, 7, 11, 15)
        QUARTER(0, 5, 10, 15)
        QUARTER(1, 6, 11, 12)
        QUARTER(2, 7, 8, 13)
        QUARTER(3, 4, 9, 14)
    }
#undef QUARTER

    for (index = 0; index < 16; index++) {
        uint32_t value = work[index] + state[index];

        out[4 * index] = (uint8_t)value;
        out[4 * index + 1] = (uint8_t)(value >> 8);
        out[4 * index + 2] = (uint8_t)(value >> 16);
        out[4 * index + 3] = (uint8_t)(value >> 24);
    }
}

static void chacha20_xor(uint8_t *out, const uint8_t *in, size_t len, const uint8_t key[32],
                         uint32_t counter, const uint8_t nonce[12])
{
    uint8_t block[64];
    size_t offset = 0;

    while (offset < len) {
        size_t chunk = len - offset > sizeof block ? sizeof block : len - offset;
        size_t index;

        chacha20_block(block, key, counter, nonce);
        counter++;
        for (index = 0; index < chunk; index++) {
            out[offset + index] = (uint8_t)(in[offset + index] ^ block[index]);
        }
        offset += chunk;
    }
}

/* -------------------------------------------------------- Poly1305 */

typedef struct {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
    uint8_t buffer[16];
    size_t buffer_len;
} poly1305_state;

static void poly1305_init(poly1305_state *state, const uint8_t key[32])
{
    uint32_t clamped[4];

    memcpy(clamped, key, sizeof clamped);
    clamped[0] &= 0x0FFFFFFFu;
    clamped[1] &= 0x0FFFFFFCu;
    clamped[2] &= 0x0FFFFFFCu;
    clamped[3] &= 0x0FFFFFFCu;

    state->r[0] = clamped[0] & POLY_MASK;
    state->r[1] = ((clamped[0] >> 26) | (clamped[1] << 6)) & POLY_MASK;
    state->r[2] = ((clamped[1] >> 20) | (clamped[2] << 12)) & POLY_MASK;
    state->r[3] = ((clamped[2] >> 14) | (clamped[3] << 18)) & POLY_MASK;
    state->r[4] = (clamped[3] >> 8) & POLY_MASK;

    state->h[0] = 0;
    state->h[1] = 0;
    state->h[2] = 0;
    state->h[3] = 0;
    state->h[4] = 0;
    state->pad[0] = load32(key + 16);
    state->pad[1] = load32(key + 20);
    state->pad[2] = load32(key + 24);
    state->pad[3] = load32(key + 28);
    state->buffer_len = 0;
}

/* Absorbs one full 16-byte block. */
static void poly1305_block(poly1305_state *state, const uint8_t *block)
{
    const uint32_t *r = state->r;
    uint32_t *h = state->h;
    unsigned __int128 d0;
    unsigned __int128 d1;
    unsigned __int128 d2;
    unsigned __int128 d3;
    unsigned __int128 d4;
    unsigned __int128 carry;

    h[0] += load32(block) & POLY_MASK;
    h[1] += (load32(block + 3) >> 2) & POLY_MASK;
    h[2] += (load32(block + 6) >> 4) & POLY_MASK;
    h[3] += (load32(block + 9) >> 6) & POLY_MASK;
    h[4] += (load32(block + 12) >> 8) | (1u << 24);

    d0 = (unsigned __int128)h[0] * r[0] + (unsigned __int128)h[1] * (5 * (unsigned __int128)r[4]) +
         (unsigned __int128)h[2] * (5 * (unsigned __int128)r[3]) +
         (unsigned __int128)h[3] * (5 * (unsigned __int128)r[2]) +
         (unsigned __int128)h[4] * (5 * (unsigned __int128)r[1]);
    d1 = (unsigned __int128)h[0] * r[1] + (unsigned __int128)h[1] * r[0] +
         (unsigned __int128)h[2] * (5 * (unsigned __int128)r[4]) +
         (unsigned __int128)h[3] * (5 * (unsigned __int128)r[3]) +
         (unsigned __int128)h[4] * (5 * (unsigned __int128)r[2]);
    d2 = (unsigned __int128)h[0] * r[2] + (unsigned __int128)h[1] * r[1] +
         (unsigned __int128)h[2] * r[0] + (unsigned __int128)h[3] * (5 * (unsigned __int128)r[4]) +
         (unsigned __int128)h[4] * (5 * (unsigned __int128)r[3]);
    d3 = (unsigned __int128)h[0] * r[3] + (unsigned __int128)h[1] * r[2] +
         (unsigned __int128)h[2] * r[1] + (unsigned __int128)h[3] * r[0] +
         (unsigned __int128)h[4] * (5 * (unsigned __int128)r[4]);
    d4 = (unsigned __int128)h[0] * r[4] + (unsigned __int128)h[1] * r[3] +
         (unsigned __int128)h[2] * r[2] + (unsigned __int128)h[3] * r[1] +
         (unsigned __int128)h[4] * r[0];

    carry = d0 >> 26;
    h[0] = (uint32_t)d0 & POLY_MASK;
    d1 += carry;
    carry = d1 >> 26;
    h[1] = (uint32_t)d1 & POLY_MASK;
    d2 += carry;
    carry = d2 >> 26;
    h[2] = (uint32_t)d2 & POLY_MASK;
    d3 += carry;
    carry = d3 >> 26;
    h[3] = (uint32_t)d3 & POLY_MASK;
    d4 += carry;
    carry = d4 >> 26;
    h[4] = (uint32_t)d4 & POLY_MASK;
    h[0] += (uint32_t)carry * 5u;
    h[1] += h[0] >> 26;
    h[0] &= POLY_MASK;
}

static void poly1305_update(poly1305_state *state, const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t take = 16 - state->buffer_len;

        if (take > len) {
            take = len;
        }
        memcpy(state->buffer + state->buffer_len, data, take);
        state->buffer_len += take;
        data += take;
        len -= take;
        if (state->buffer_len == sizeof state->buffer) {
            poly1305_block(state, state->buffer);
            state->buffer_len = 0;
        }
    }
}

static void poly1305_finish(poly1305_state *state, uint8_t tag[16])
{
    uint32_t h[5];
    uint32_t g[5];
    uint32_t mask;
    uint32_t word[4];
    uint64_t sum;
    size_t index;

    if (state->buffer_len > 0) {
        uint8_t block[16];

        memset(block, 0, sizeof block);
        memcpy(block, state->buffer, state->buffer_len);
        block[state->buffer_len] = 1;
        poly1305_block(state, block);
    }

    for (index = 0; index < 5; index++) {
        h[index] = state->h[index];
    }
    /* fully carry h */
    h[1] += h[0] >> 26;
    h[0] &= POLY_MASK;
    h[2] += h[1] >> 26;
    h[1] &= POLY_MASK;
    h[3] += h[2] >> 26;
    h[2] &= POLY_MASK;
    h[4] += h[3] >> 26;
    h[3] &= POLY_MASK;
    h[0] += (h[4] >> 26) * 5u;
    h[4] &= POLY_MASK;
    h[1] += h[0] >> 26;
    h[0] &= POLY_MASK;

    /* g = h + 5 - 2^130, keeping it only when h >= p */
    g[0] = h[0] + 5u;
    g[1] = h[1] + (g[0] >> 26);
    g[0] &= POLY_MASK;
    g[2] = h[2] + (g[1] >> 26);
    g[1] &= POLY_MASK;
    g[3] = h[3] + (g[2] >> 26);
    g[2] &= POLY_MASK;
    g[4] = h[4] + (g[3] >> 26);
    g[3] &= POLY_MASK;
    g[4] -= 1u << 26;
    mask = (g[4] >> 31) - 1u; /* all ones when the subtraction did not underflow */
    for (index = 0; index < 5; index++) {
        g[index] &= mask;
        h[index] = (h[index] & ~mask) | g[index];
    }

    /* fold to 128 bits */
    word[0] = (h[0] | (h[1] << 26));
    word[1] = ((h[1] >> 6) | (h[2] << 20));
    word[2] = ((h[2] >> 12) | (h[3] << 14));
    word[3] = ((h[3] >> 18) | (h[4] << 8));

    sum = (uint64_t)word[0] + state->pad[0];
    word[0] = (uint32_t)sum;
    sum = (uint64_t)word[1] + state->pad[1] + (sum >> 32);
    word[1] = (uint32_t)sum;
    sum = (uint64_t)word[2] + state->pad[2] + (sum >> 32);
    word[2] = (uint32_t)sum;
    sum = (uint64_t)word[3] + state->pad[3] + (sum >> 32);
    word[3] = (uint32_t)sum;

    for (index = 0; index < 4; index++) {
        tag[4 * index] = (uint8_t)word[index];
        tag[4 * index + 1] = (uint8_t)(word[index] >> 8);
        tag[4 * index + 2] = (uint8_t)(word[index] >> 16);
        tag[4 * index + 3] = (uint8_t)(word[index] >> 24);
    }
}

/* ------------------------------------------------- ChaCha20-Poly1305 AEAD */

static void aead_tag(uint8_t tag[16], const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len, const uint8_t *ciphertext,
                     size_t ciphertext_len)
{
    poly1305_state state;
    uint8_t one_time_key[64];
    uint8_t padding[16] = {0};
    uint8_t lengths[16];
    size_t index;

    chacha20_block(one_time_key, key, 0, nonce);
    poly1305_init(&state, one_time_key);

    poly1305_update(&state, aad, aad_len);
    poly1305_update(&state, padding, (16 - aad_len % 16) % 16);
    poly1305_update(&state, ciphertext, ciphertext_len);
    poly1305_update(&state, padding, (16 - ciphertext_len % 16) % 16);
    for (index = 0; index < 8; index++) {
        lengths[index] = (uint8_t)((uint64_t)aad_len >> (8 * index));
        lengths[8 + index] = (uint8_t)((uint64_t)ciphertext_len >> (8 * index));
    }
    poly1305_update(&state, lengths, sizeof lengths);
    poly1305_finish(&state, tag);
}

psa_status_t psa_aead_encrypt(mbedtls_svc_key_id_t key, psa_algorithm_t alg, const uint8_t *nonce,
                              size_t nonce_length, const uint8_t *additional_data,
                              size_t additional_data_length, const uint8_t *plaintext,
                              size_t plaintext_length, uint8_t *ciphertext,
                              size_t ciphertext_size, size_t *ciphertext_length)
{
    const key_slot_t *slot;

    if (key == 0 || key > KEY_SLOTS || !slots[key - 1].in_use ||
        alg != PSA_ALG_CHACHA20_POLY1305 || nonce_length != 12 ||
        ciphertext_size < plaintext_length + 16) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    slot = &slots[key - 1];
    if (slot->usage != PSA_KEY_USAGE_ENCRYPT) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    chacha20_xor(ciphertext, plaintext, plaintext_length, slot->key, 1, nonce);
    aead_tag(ciphertext + plaintext_length, slot->key, nonce, additional_data,
             additional_data_length, ciphertext, plaintext_length);
    *ciphertext_length = plaintext_length + 16;
    return PSA_SUCCESS;
}

psa_status_t psa_aead_decrypt(mbedtls_svc_key_id_t key, psa_algorithm_t alg, const uint8_t *nonce,
                              size_t nonce_length, const uint8_t *additional_data,
                              size_t additional_data_length, const uint8_t *ciphertext,
                              size_t ciphertext_length, uint8_t *plaintext,
                              size_t plaintext_size, size_t *plaintext_length)
{
    const key_slot_t *slot;
    uint8_t expected[16];

    if (ciphertext_length < 16) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    if (key == 0 || key > KEY_SLOTS || !slots[key - 1].in_use ||
        alg != PSA_ALG_CHACHA20_POLY1305 || nonce_length != 12 ||
        plaintext_size < ciphertext_length - 16) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    slot = &slots[key - 1];
    if (slot->usage != PSA_KEY_USAGE_DECRYPT) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    aead_tag(expected, slot->key, nonce, additional_data, additional_data_length, ciphertext,
             ciphertext_length - 16);
    if (memcmp(expected, ciphertext + ciphertext_length - 16, 16) != 0) {
        return PSA_ERROR_INVALID_SIGNATURE;
    }
    chacha20_xor(plaintext, ciphertext, ciphertext_length - 16, slot->key, 1, nonce);
    *plaintext_length = ciphertext_length - 16;
    return PSA_SUCCESS;
}
