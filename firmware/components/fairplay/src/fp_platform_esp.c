/*
 * Platform primitives for ESP-IDF: SHA-512 and HMAC-SHA1 through PSA crypto.
 *
 * These are the only two operations the FairPlay component needs from the
 * platform:
 *
 *   SHA-512      psa_hash_compute(PSA_ALG_SHA_512, ...)      - one shot, raw input
 *   HMAC-SHA1    psa_mac_compute(..., PSA_ALG_HMAC(PSA_ALG_SHA_1), ...)
 *
 * psa_mac_compute takes a key identifier rather than raw key bytes, so the
 * 20-byte FairPlay MAC key is imported as a volatile HMAC key, used and then
 * destroyed. That is the portable PSA 1.0/1.1 way of doing a one-shot HMAC with
 * caller-supplied key material (the older raw-buffer signature does not exist
 * in current PSA headers).
 *
 * MD5 and AES-CBC are deliberately NOT taken from the platform: FairPlay's
 * variants differ from the standards (big-endian message words plus a mutated
 * schedule for MD5, injected round keys for the message cipher), so both live
 * in fp_md5.c and fp_message.c.
 *
 * PSA initialisation:
 *   psa_crypto_init() must run before any other PSA call. It is idempotent, so
 *   this file calls it on entry instead of caching an "already initialised"
 *   flag: the component then owns no shared mutable state and cannot race when
 *   two tasks run the handshake at the same time. An application that already
 *   initialises PSA during start-up pays nothing extra here.
 *
 * Configuration requirements (ESP-IDF component config, not this component):
 *   CONFIG_MBEDTLS_PSA_CRYPTO_C  PSA crypto enabled (on by default)
 *   MBEDTLS_SHA512_C             PSA_WANT_ALG_SHA_512  - stream key mixing
 *   MBEDTLS_SHA1_C, HMAC         PSA_WANT_ALG_SHA_1 + PSA_WANT_ALG_HMAC - ekey tag
 */
#include "psa/crypto.h"

#include "fp_internal.h"

/* PSA 1.1 renamed the usage flag required by psa_mac_compute from
 * SIGN_HASH to SIGN_MESSAGE. Pick whichever the header provides. */
#if defined(PSA_KEY_USAGE_SIGN_MESSAGE)
#define FP_PSA_MAC_USAGE PSA_KEY_USAGE_SIGN_MESSAGE
#else
#define FP_PSA_MAC_USAGE PSA_KEY_USAGE_SIGN_HASH
#endif

int fp_sha512(const uint8_t *data, size_t len, uint8_t out[FP_SHA512_LEN])
{
    size_t produced = 0;

    if (out == NULL || (data == NULL && len != 0)) {
        return FP_ERR_ARG;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return FP_ERR_CRYPTO;
    }
    /* psa_hash_compute reads input_length bytes; an empty message is passed as a
     * pointer to an empty string so a NULL never reaches the library. */
    const uint8_t *input = (data != NULL) ? data : (const uint8_t *)"";
    if (psa_hash_compute(PSA_ALG_SHA_512, input, len, out, FP_SHA512_LEN, &produced) != PSA_SUCCESS) {
        return FP_ERR_CRYPTO;
    }
    return produced == FP_SHA512_LEN ? FP_OK : FP_ERR_CRYPTO;
}

int fp_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                 uint8_t out[FP_SHA1_LEN])
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    size_t produced = 0;
    int result = FP_OK;

    if (key == NULL || key_len == 0 || out == NULL || (msg == NULL && msg_len != 0)) {
        return FP_ERR_ARG;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return FP_ERR_CRYPTO;
    }

    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, key_len * 8);
    psa_set_key_usage_flags(&attributes, FP_PSA_MAC_USAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_1));
    if (psa_import_key(&attributes, key, key_len, &key_id) != PSA_SUCCESS) {
        /* Nothing was imported, so there is no key to destroy. */
        psa_reset_key_attributes(&attributes);
        return FP_ERR_CRYPTO;
    }
    psa_reset_key_attributes(&attributes);

    const uint8_t *input = (msg != NULL) ? msg : (const uint8_t *)"";
    if (psa_mac_compute(key_id, PSA_ALG_HMAC(PSA_ALG_SHA_1), input, msg_len, out, FP_SHA1_LEN, &produced) !=
            PSA_SUCCESS ||
        produced != FP_SHA1_LEN) {
        result = FP_ERR_CRYPTO;
    }
    /* The key only exists for this one MAC; drop it even if the MAC failed. */
    if (psa_destroy_key(key_id) != PSA_SUCCESS) {
        result = FP_ERR_CRYPTO;
    }
    return result;
}
