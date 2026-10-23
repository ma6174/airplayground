/*
 * HKDF-SHA512 through PSA crypto (mbedTLS 4 exposes no other public API).
 *
 * Every label used by the pairing protocol lives here, including the two that
 * are not symmetric with the control channel: the event channel swaps
 * write/read because the receiver initiates on that connection, and the data
 * stream derives one direction only.
 */
#include "hap_internal.h"

#include <stdio.h>
#include <string.h>

#include "psa/crypto.h"

static bool psa_ready;

int hap_psa_ensure_init(void)
{
    psa_status_t status;

    if (psa_ready) {
        return 0;
    }
    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        HAP_LOGE("psa: init failed (%ld)", (long)status);
        return -1;
    }
    psa_ready = true;
    return 0;
}

int hap_hkdf_sha512(const uint8_t *ikm, size_t ikm_len, const char *salt, const char *info,
                    uint8_t *out, size_t out_len)
{
    psa_key_derivation_operation_t operation = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status;

    if (ikm == NULL || ikm_len == 0 || salt == NULL || info == NULL || out == NULL ||
        out_len == 0) {
        return -1;
    }
    if (hap_psa_ensure_init() != 0) {
        return -1;
    }

    status = psa_key_derivation_setup(&operation, PSA_ALG_HKDF(PSA_ALG_SHA_512));
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SALT,
                                                (const uint8_t *)salt, strlen(salt));
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SECRET, ikm,
                                                ikm_len);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_INFO,
                                                (const uint8_t *)info, strlen(info));
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_output_bytes(&operation, out, out_len);
    }
    psa_key_derivation_abort(&operation);

    if (status != PSA_SUCCESS) {
        HAP_LOGE("hkdf(%s): failed (%ld)", info, (long)status);
        memset(out, 0, out_len);
        return -1;
    }
    return 0;
}

void hap_derive_control_keys(const uint8_t shared[HAP_SHARED_SECRET_LEN],
                             uint8_t write_key[HAP_KEY_LEN], uint8_t read_key[HAP_KEY_LEN])
{
    if (hap_hkdf_sha512(shared, HAP_SHARED_SECRET_LEN, HAP_HKDF_SALT_CONTROL,
                        HAP_HKDF_INFO_CONTROL_WRITE, write_key, HAP_KEY_LEN) != 0) {
        memset(write_key, 0, HAP_KEY_LEN);
    }
    if (hap_hkdf_sha512(shared, HAP_SHARED_SECRET_LEN, HAP_HKDF_SALT_CONTROL,
                        HAP_HKDF_INFO_CONTROL_READ, read_key, HAP_KEY_LEN) != 0) {
        memset(read_key, 0, HAP_KEY_LEN);
    }
}

void hap_derive_event_keys(const uint8_t shared[HAP_SHARED_SECRET_LEN],
                           uint8_t write_key[HAP_KEY_LEN], uint8_t read_key[HAP_KEY_LEN])
{
    if (hap_hkdf_sha512(shared, HAP_SHARED_SECRET_LEN, HAP_HKDF_SALT_EVENT,
                        HAP_HKDF_INFO_EVENT_READ, write_key, HAP_KEY_LEN) != 0) {
        memset(write_key, 0, HAP_KEY_LEN);
    }
    if (hap_hkdf_sha512(shared, HAP_SHARED_SECRET_LEN, HAP_HKDF_SALT_EVENT,
                        HAP_HKDF_INFO_EVENT_WRITE, read_key, HAP_KEY_LEN) != 0) {
        memset(read_key, 0, HAP_KEY_LEN);
    }
}

int hap_derive_data_key(const uint8_t *ikm, size_t ikm_len, uint64_t stream_connection_id,
                        uint8_t out_key[HAP_KEY_LEN])
{
    char salt[40];

    if (ikm == NULL || ikm_len == 0 || out_key == NULL) {
        return -1;
    }
    snprintf(salt, sizeof salt, "%s%llu", HAP_HKDF_SALT_DATASTREAM,
             (unsigned long long)stream_connection_id);
    return hap_hkdf_sha512(ikm, ikm_len, salt, HAP_HKDF_INFO_DATASTREAM, out_key, HAP_KEY_LEN);
}
