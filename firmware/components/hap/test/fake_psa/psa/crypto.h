/*
 * Test-only stand-in for the slice of <psa/crypto.h> that the HAP component
 * uses. The device build compiles against mbedTLS 4.1 (see the `syntax` target
 * of the Makefile, which type-checks the real headers); this header lets the
 * host tests link and run the pair-setup, pair-verify, KDF and framing code with
 * a small independent implementation behind it.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int32_t psa_status_t;
typedef uint32_t psa_algorithm_t;
typedef uint32_t psa_key_usage_t;
typedef uint32_t psa_key_derivation_step_t;
typedef uint32_t mbedtls_svc_key_id_t;
typedef uint32_t psa_key_type_t;

#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ERROR_INVALID_ARGUMENT ((psa_status_t)-135)
#define PSA_ERROR_NOT_SUPPORTED ((psa_status_t)-134)
#define PSA_ERROR_INVALID_SIGNATURE ((psa_status_t)-149)
#define PSA_ERROR_BUFFER_TOO_SMALL ((psa_status_t)-138)
#define PSA_ERROR_INSUFFICIENT_STORAGE ((psa_status_t)-141)

#define PSA_KEY_TYPE_CHACHA20 ((psa_key_type_t)0x2004)
#define PSA_ALG_CHACHA20_POLY1305 ((psa_algorithm_t)0x05100500)
#define PSA_ALG_SHA_512 ((psa_algorithm_t)0x0200000a)
#define PSA_ALG_HKDF(hash) ((psa_algorithm_t)(0x08000000 | ((hash) & 0x000000ff)))

#define PSA_KEY_USAGE_ENCRYPT ((psa_key_usage_t)0x00000100)
#define PSA_KEY_USAGE_DECRYPT ((psa_key_usage_t)0x00000200)

#define PSA_KEY_DERIVATION_INPUT_SECRET ((psa_key_derivation_step_t)0x0201)
#define PSA_KEY_DERIVATION_INPUT_SALT ((psa_key_derivation_step_t)0x0202)
#define PSA_KEY_DERIVATION_INPUT_INFO ((psa_key_derivation_step_t)0x0203)

typedef struct {
    psa_key_type_t type;
    size_t bits;
    psa_key_usage_t usage;
    psa_algorithm_t algorithm;
} psa_key_attributes_t;

typedef struct {
    psa_algorithm_t algorithm;
    uint8_t salt[64];
    size_t salt_len;
    uint8_t secret[64];
    size_t secret_len;
    uint8_t info[64];
    size_t info_len;
    int have_salt;
    int have_secret;
    int have_info;
} psa_key_derivation_operation_t;

#define PSA_KEY_DERIVATION_OPERATION_INIT \
    { 0, {0}, 0, {0}, 0, {0}, 0, 0, 0, 0 }

static inline psa_key_attributes_t psa_key_attributes_init(void)
{
    psa_key_attributes_t attributes;

    attributes.type = 0;
    attributes.bits = 0;
    attributes.usage = 0;
    attributes.algorithm = 0;
    return attributes;
}

static inline void psa_set_key_type(psa_key_attributes_t *attributes, psa_key_type_t type)
{
    attributes->type = type;
}

static inline void psa_set_key_bits(psa_key_attributes_t *attributes, size_t bits)
{
    attributes->bits = bits;
}

static inline void psa_set_key_usage_flags(psa_key_attributes_t *attributes, psa_key_usage_t usage)
{
    attributes->usage = usage;
}

static inline void psa_set_key_algorithm(psa_key_attributes_t *attributes, psa_algorithm_t algorithm)
{
    attributes->algorithm = algorithm;
}

void psa_reset_key_attributes(psa_key_attributes_t *attributes);

psa_status_t psa_crypto_init(void);
psa_status_t psa_key_derivation_setup(psa_key_derivation_operation_t *operation,
                                      psa_algorithm_t algorithm);
psa_status_t psa_key_derivation_input_bytes(psa_key_derivation_operation_t *operation,
                                            psa_key_derivation_step_t step, const uint8_t *data,
                                            size_t data_length);
psa_status_t psa_key_derivation_output_bytes(psa_key_derivation_operation_t *operation,
                                             uint8_t *output, size_t output_length);
psa_status_t psa_key_derivation_abort(psa_key_derivation_operation_t *operation);
psa_status_t psa_import_key(const psa_key_attributes_t *attributes, const uint8_t *data,
                            size_t data_length, mbedtls_svc_key_id_t *key);
psa_status_t psa_destroy_key(mbedtls_svc_key_id_t key);
psa_status_t psa_aead_encrypt(mbedtls_svc_key_id_t key, psa_algorithm_t alg, const uint8_t *nonce,
                              size_t nonce_length, const uint8_t *additional_data,
                              size_t additional_data_length, const uint8_t *plaintext,
                              size_t plaintext_length, uint8_t *ciphertext,
                              size_t ciphertext_size, size_t *ciphertext_length);
psa_status_t psa_aead_decrypt(mbedtls_svc_key_id_t key, psa_algorithm_t alg, const uint8_t *nonce,
                              size_t nonce_length, const uint8_t *additional_data,
                              size_t additional_data_length, const uint8_t *ciphertext,
                              size_t ciphertext_length, uint8_t *plaintext,
                              size_t plaintext_size, size_t *plaintext_length);
