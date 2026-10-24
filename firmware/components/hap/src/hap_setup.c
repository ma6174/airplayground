/*
 * HAP pair-setup: M1 (method/state/flags) -> M2 (salt + server public key)
 * -> M3 (client public key + proof) -> M4 (server proof) -> M5 (encrypted
 * Ed25519 identity) -> M6 (acknowledgement).
 *
 * The transport is the caller's problem: each step consumes the received TLV8
 * message and produces the next one.
 */
#include "hap_internal.h"

#include <stdlib.h>
#include <string.h>

/* pair-setup flags: 0x10 marks transient (no-PIN) pairing. */
#define HAP_FLAG_TRANSIENT 0x00000010U

#define HAP_SALT_MAX 128
#define HAP_SUBTLV_MAX 256
#define HAP_SETUP_PIN_MAX 15

enum {
    HAP_SETUP_CREATED = 0,
    HAP_SETUP_SENT_M1,
    HAP_SETUP_SENT_M3,
    HAP_SETUP_SENT_M5,
    HAP_SETUP_COMPLETE,
};

struct hap_setup {
    hap_identity_t *identity;
    hap_srp_t srp;
    char pin[HAP_SETUP_PIN_MAX + 1];
    bool transient;
    bool include_acl;
    int state;
};

static const char *tlv_error_name(int code)
{
    switch (code) {
    case 1:
        return "unknown";
    case 2:
        return "authentication";
    case 3:
        return "backoff";
    case 4:
        return "maximum peers";
    case 5:
        return "maximum tries";
    case 6:
        return "unavailable";
    case 7:
        return "busy";
    default:
        return "unexpected";
    }
}

hap_setup_t *hap_setup_new(hap_identity_t *identity, const char *pin, bool include_acl)
{
    hap_setup_t *setup;

    if (identity == NULL) {
        return NULL;
    }
    setup = calloc(1, sizeof *setup);
    if (setup == NULL) {
        HAP_LOGE("setup: out of memory");
        return NULL;
    }
    setup->identity = identity;
    setup->transient = (pin == NULL || pin[0] == '\0');
    setup->include_acl = include_acl && !setup->transient;
    if (!setup->transient) {
        if (strlen(pin) > HAP_SETUP_PIN_MAX) {
            HAP_LOGE("setup: pairing code is longer than %d characters", HAP_SETUP_PIN_MAX);
            free(setup);
            return NULL;
        }
        strcpy(setup->pin, pin);
    }
    return setup;
}

void hap_setup_free(hap_setup_t *setup)
{
    if (setup == NULL) {
        return;
    }
    memset(setup, 0, sizeof *setup);
    free(setup);
}

int hap_setup_begin(hap_setup_t *setup, uint8_t *out, size_t out_cap, size_t *out_len)
{
    hap_tlv_out_t message;

    if (setup == NULL || out == NULL || out_len == NULL || setup->state != HAP_SETUP_CREATED) {
        return -1;
    }
    hap_tlv_out_init(&message, out, out_cap);
    hap_tlv_put_u8(&message, HAP_TLV_METHOD, 0x00);
    hap_tlv_put_u8(&message, HAP_TLV_STATE, 0x01);
    if (setup->transient) {
        /* Transient pairing is the only flow that carries the flags TLV; the
         * receiver rejects a request that asks for it otherwise. */
        const uint8_t flags[4] = {(uint8_t)(HAP_FLAG_TRANSIENT & 0xFFu), 0x00, 0x00, 0x00};
        hap_tlv_put(&message, HAP_TLV_FLAGS, flags, sizeof flags);
    }
    if (message.overflow) {
        return -1;
    }
    setup->state = HAP_SETUP_SENT_M1;
    *out_len = message.len;
    HAP_LOGI("setup: M1 sent (%s)", setup->transient ? "transient" : "pin");
    return 0;
}

int hap_setup_m2(hap_setup_t *setup, const uint8_t *in, size_t in_len, uint8_t *out,
                 size_t out_cap, size_t *out_len)
{
    uint8_t salt[HAP_SALT_MAX];
    size_t salt_len = 0;
    uint8_t server_public[HAP_SRP_WIDTH];
    size_t server_public_len = 0;
    hap_tlv_out_t message;
    int error;

    if (setup == NULL || in == NULL || out == NULL || out_len == NULL ||
        setup->state != HAP_SETUP_SENT_M1) {
        return -1;
    }
    error = hap_tlv_error(in, in_len);
    if (error >= 0) {
        /* Backoff (3) is a transport concern: the caller re-sends M1 after the
         * advertised delay. */
        HAP_LOGE("setup: M2 rejected with %s (%d)", tlv_error_name(error), error);
        return -1;
    }
    if (hap_tlv_get(in, in_len, HAP_TLV_SALT, salt, sizeof salt, &salt_len) != 0 || salt_len == 0 ||
        hap_tlv_get(in, in_len, HAP_TLV_PUBLIC_KEY, server_public, sizeof server_public,
                    &server_public_len) != 0 ||
        server_public_len == 0) {
        HAP_LOGE("setup: M2 is missing the salt or the server public key");
        return -1;
    }

    if (hap_srp_client_start(&setup->srp, setup->pin, salt, salt_len, server_public,
                             server_public_len, NULL) != 0) {
        HAP_LOGE("setup: SRP failed");
        return -1;
    }

    hap_tlv_out_init(&message, out, out_cap);
    hap_tlv_put_u8(&message, HAP_TLV_STATE, 0x03);
    hap_tlv_put(&message, HAP_TLV_PUBLIC_KEY, setup->srp.padded_a, HAP_SRP_WIDTH);
    hap_tlv_put(&message, HAP_TLV_PROOF, setup->srp.m1, sizeof setup->srp.m1);
    if (message.overflow) {
        return -1;
    }
    setup->state = HAP_SETUP_SENT_M3;
    *out_len = message.len;
    HAP_LOGI("setup: M3 sent (A + proof)");
    return 0;
}

int hap_setup_m4(hap_setup_t *setup, const uint8_t *in, size_t in_len, uint8_t *out,
                 size_t out_cap, size_t *out_len)
{
    uint8_t proof[HAP_SRP_HASH_LEN];
    size_t proof_len = 0;
    uint8_t session_key[HAP_KEY_LEN];
    uint8_t sign_key[HAP_KEY_LEN];
    uint8_t signature[HAP_ED25519_SIG_LEN];
    uint8_t sign_input[HAP_KEY_LEN + HAP_ID_LEN + HAP_ED25519_PUB_LEN];
    uint8_t sub_tlv_buf[HAP_SUBTLV_MAX];
    uint8_t encrypted[HAP_SUBTLV_MAX + 16];
    hap_tlv_out_t sub;
    hap_tlv_out_t message;
    size_t sign_input_len = 0;
    int error;

    if (setup == NULL || in == NULL || out == NULL || out_len == NULL ||
        setup->state != HAP_SETUP_SENT_M3) {
        return -1;
    }
    error = hap_tlv_error(in, in_len);
    if (error >= 0) {
        HAP_LOGE("setup: M4 rejected with %s (%d)", tlv_error_name(error), error);
        return -1;
    }
    if (hap_tlv_get(in, in_len, HAP_TLV_PROOF, proof, sizeof proof, &proof_len) != 0) {
        HAP_LOGE("setup: M4 has no proof TLV");
        return -1;
    }
    if (proof_len == 0) {
        /* Some receivers omit the server proof; the exchange continues, as in
         * the reference sender. */
        HAP_LOGW("setup: M4 carried no server proof");
    } else if (hap_srp_verify_server_proof(&setup->srp, proof, proof_len) != 0) {
            return -1;
    } else {
        HAP_LOGI("setup: server proof verified");
    }

    if (hap_hkdf_sha512(setup->srp.k, sizeof setup->srp.k, HAP_HKDF_SALT_SETUP_ENCRYPT,
                        HAP_HKDF_INFO_SETUP_ENCRYPT, session_key, sizeof session_key) != 0 ||
        hap_hkdf_sha512(setup->srp.k, sizeof setup->srp.k, HAP_HKDF_SALT_SETUP_SIGN,
                        HAP_HKDF_INFO_SETUP_SIGN, sign_key, sizeof sign_key) != 0) {
        HAP_LOGE("setup: cannot derive the setup keys");
        return -1;
    }

    /* signature over sigKey || PairingID || Ed25519 public key */
    memcpy(sign_input + sign_input_len, sign_key, sizeof sign_key);
    sign_input_len += sizeof sign_key;
    memcpy(sign_input + sign_input_len, setup->identity->pairing_id,
           strlen(setup->identity->pairing_id));
    sign_input_len += strlen(setup->identity->pairing_id);
    memcpy(sign_input + sign_input_len, setup->identity->public_key,
           sizeof setup->identity->public_key);
    sign_input_len += sizeof setup->identity->public_key;

    hap_identity_sign(setup->identity, sign_input, sign_input_len, signature);

    hap_tlv_out_init(&sub, sub_tlv_buf, sizeof sub_tlv_buf);
    hap_tlv_put(&sub, HAP_TLV_IDENTIFIER, (const uint8_t *)setup->identity->pairing_id,
                strlen(setup->identity->pairing_id));
    hap_tlv_put(&sub, HAP_TLV_PUBLIC_KEY, setup->identity->public_key,
                sizeof setup->identity->public_key);
    hap_tlv_put(&sub, HAP_TLV_SIGNATURE, signature, sizeof signature);
    if (setup->include_acl) {
        /* Screen-capture pairing only: receivers reject such an identity
         * without the ACL request. */
        hap_tlv_put(&sub, HAP_TLV_ACL, (const uint8_t *)HAP_ACL_SCREEN_CAPTURE,
                    HAP_ACL_SCREEN_CAPTURE_LEN);
    }
    if (sub.overflow || sub.len > sizeof encrypted - 16) {
        HAP_LOGE("setup: identity sub-TLV does not fit (%u bytes)", (unsigned)sub.len);
        return -1;
    }

    if (hap_aead_seal_label(session_key, "PS-Msg05", NULL, 0, sub_tlv_buf, sub.len, encrypted) !=
        0) {
        HAP_LOGE("setup: cannot seal the identity");
        return -1;
    }

    hap_tlv_out_init(&message, out, out_cap);
    hap_tlv_put(&message, HAP_TLV_ENCRYPTED_DATA, encrypted, sub.len + 16);
    hap_tlv_put_u8(&message, HAP_TLV_STATE, 0x05);
    if (message.overflow) {
        HAP_LOGE("setup: M5 does not fit the output buffer");
        return -1;
    }
    setup->state = HAP_SETUP_SENT_M5;
    *out_len = message.len;
    HAP_LOGI("setup: M5 sent (encrypted identity, %u byte sub-TLV)", (unsigned)sub.len);
    return 0;
}

int hap_setup_m6(hap_setup_t *setup, const uint8_t *in, size_t in_len)
{
    int error;

    if (setup == NULL || in == NULL || setup->state != HAP_SETUP_SENT_M5) {
        return -1;
    }
    error = hap_tlv_error(in, in_len);
    if (error >= 0) {
        HAP_LOGE("setup: M6 rejected with %s (%d)", tlv_error_name(error), error);
        return -1;
    }
    setup->state = HAP_SETUP_COMPLETE;
    HAP_LOGI("setup: complete");
    return 0;
}
