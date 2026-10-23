/*
 * HAP pair-verify: V1 (ephemeral X25519 public key) -> V2 (server ephemeral key,
 * optionally encrypted) -> V3 (encrypted Ed25519 signature) -> V4 (ack).
 *
 * The X25519 shared secret computed here is the root of the control, event and
 * data stream keys, and it replaces the SRP secret for everything after
 * pairing.
 */
#include "hap_internal.h"

#include <stdlib.h>
#include <string.h>

#define HAP_SUBTLV_MAX 128

enum {
    HAP_VERIFY_CREATED = 0,
    HAP_VERIFY_SENT_V1,
    HAP_VERIFY_SENT_V3,
    HAP_VERIFY_COMPLETE,
};

struct hap_verify {
    hap_identity_t *identity;
    uint8_t private_key[32];
    uint8_t public_key[32];
    uint8_t peer_key[32];
    uint8_t shared[HAP_SHARED_SECRET_LEN];
    uint8_t verify_key[HAP_KEY_LEN];
    bool have_shared;
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

hap_verify_t *hap_verify_new(hap_identity_t *identity)
{
    hap_verify_t *verify;

    if (identity == NULL) {
        return NULL;
    }
    verify = calloc(1, sizeof *verify);
    if (verify == NULL) {
        HAP_LOGE("verify: out of memory");
        return NULL;
    }
    verify->identity = identity;
    return verify;
}

void hap_verify_free(hap_verify_t *verify)
{
    if (verify == NULL) {
        return;
    }
    memset(verify, 0, sizeof *verify);
    free(verify);
}

int hap_verify_begin(hap_verify_t *verify, uint8_t *out, size_t out_cap, size_t *out_len)
{
    hap_tlv_out_t message;

    if (verify == NULL || out == NULL || out_len == NULL || verify->state != HAP_VERIFY_CREATED) {
        return -1;
    }

    randombytes(verify->private_key, sizeof verify->private_key);
    hap_x25519_public_key(verify->public_key, verify->private_key);

    hap_tlv_out_init(&message, out, out_cap);
    hap_tlv_put_u8(&message, HAP_TLV_STATE, 0x01);
    hap_tlv_put(&message, HAP_TLV_PUBLIC_KEY, verify->public_key, sizeof verify->public_key);
    if (message.overflow) {
        return -1;
    }
    verify->state = HAP_VERIFY_SENT_V1;
    *out_len = message.len;
    HAP_LOGI("verify: V1 sent (ephemeral X25519 key)");
    return 0;
}

int hap_verify_v2(hap_verify_t *verify, const uint8_t *in, size_t in_len, uint8_t *out,
                  size_t out_cap, size_t *out_len)
{
    uint8_t server_public[HAP_SRP_WIDTH];
    size_t server_public_len = 0;
    uint8_t encrypted[HAP_SUBTLV_MAX + 16];
    size_t encrypted_len = 0;
    uint8_t plain[HAP_SUBTLV_MAX];
    uint8_t signature[HAP_ED25519_SIG_LEN];
    uint8_t sign_input[32 + HAP_ID_LEN + 32];
    uint8_t sub_tlv_buf[HAP_SUBTLV_MAX];
    size_t sign_input_len = 0;
    size_t id_len;
    hap_tlv_out_t sub;
    hap_tlv_out_t message;
    int error;

    if (verify == NULL || in == NULL || out == NULL || out_len == NULL ||
        verify->state != HAP_VERIFY_SENT_V1) {
        return -1;
    }
    error = hap_tlv_error(in, in_len);
    if (error >= 0) {
        HAP_LOGE("verify: V2 rejected with %s (%d)", tlv_error_name(error), error);
        return -1;
    }
    if (hap_tlv_get(in, in_len, HAP_TLV_PUBLIC_KEY, server_public, sizeof server_public,
                    &server_public_len) != 0) {
        return -1;
    }
    if (server_public_len < sizeof verify->peer_key) {
        HAP_LOGE("verify: V2 server public key is %u bytes", (unsigned)server_public_len);
        return -1;
    }
    memcpy(verify->peer_key, server_public, sizeof verify->peer_key);

    if (hap_x25519_shared(verify->shared, verify->private_key, verify->peer_key) != 0) {
        return -1;
    }
    verify->have_shared = true;

    if (hap_hkdf_sha512(verify->shared, sizeof verify->shared, HAP_HKDF_SALT_VERIFY,
                        HAP_HKDF_INFO_VERIFY, verify->verify_key, sizeof verify->verify_key) != 0) {
        return -1;
    }

    if (hap_tlv_get(in, in_len, HAP_TLV_ENCRYPTED_DATA, encrypted, sizeof encrypted,
                    &encrypted_len) != 0) {
        return -1;
    }
    if (encrypted_len > 0) {
        /* Receivers that answer V1 with an encrypted sub-TLV prove the shared
         * secret before the client reveals its identity. */
        if (encrypted_len <= 16 || encrypted_len > sizeof plain + 16) {
            HAP_LOGE("verify: V2 encrypted payload is %u bytes", (unsigned)encrypted_len);
            return -1;
        }
        if (hap_aead_open_label(verify->verify_key, "PV-Msg02", NULL, 0, encrypted, encrypted_len,
                                plain) != 0) {
            HAP_LOGE("verify: V2 payload did not authenticate");
            return -1;
        }
        HAP_LOGD("verify: V2 payload authenticated (%u bytes)", (unsigned)(encrypted_len - 16));
    }

    /* signature over clientX25519Public || PairingID || serverX25519Public */
    id_len = strlen(verify->identity->pairing_id);
    memcpy(sign_input + sign_input_len, verify->public_key, sizeof verify->public_key);
    sign_input_len += sizeof verify->public_key;
    memcpy(sign_input + sign_input_len, verify->identity->pairing_id, id_len);
    sign_input_len += id_len;
    memcpy(sign_input + sign_input_len, verify->peer_key, sizeof verify->peer_key);
    sign_input_len += sizeof verify->peer_key;

    hap_identity_sign(verify->identity, sign_input, sign_input_len, signature);

    hap_tlv_out_init(&sub, sub_tlv_buf, sizeof sub_tlv_buf);
    hap_tlv_put(&sub, HAP_TLV_IDENTIFIER, (const uint8_t *)verify->identity->pairing_id, id_len);
    hap_tlv_put(&sub, HAP_TLV_SIGNATURE, signature, sizeof signature);
    if (sub.overflow || sub.len > sizeof encrypted - 16) {
        return -1;
    }

    if (hap_aead_seal_label(verify->verify_key, "PV-Msg03", NULL, 0, sub_tlv_buf, sub.len,
                            encrypted) != 0) {
        return -1;
    }

    hap_tlv_out_init(&message, out, out_cap);
    hap_tlv_put_u8(&message, HAP_TLV_STATE, 0x03);
    hap_tlv_put(&message, HAP_TLV_ENCRYPTED_DATA, encrypted, sub.len + 16);
    if (message.overflow) {
        return -1;
    }
    verify->state = HAP_VERIFY_SENT_V3;
    *out_len = message.len;
    HAP_LOGI("verify: V3 sent (signature over both ephemeral keys)");
    return 0;
}

int hap_verify_v4(hap_verify_t *verify, const uint8_t *in, size_t in_len)
{
    int error;

    if (verify == NULL || verify->state != HAP_VERIFY_SENT_V3) {
        return -1;
    }
    if (in_len > 0) {
        if (in == NULL) {
            return -1;
        }
        error = hap_tlv_error(in, in_len);
        if (error >= 0) {
            HAP_LOGE("verify: V4 rejected with %s (%d)", tlv_error_name(error), error);
            return -1;
        }
    }
    verify->state = HAP_VERIFY_COMPLETE;
    HAP_LOGI("verify: complete, control channel is now encrypted");
    return 0;
}

const uint8_t *hap_verify_shared_secret(const hap_verify_t *verify)
{
    if (verify == NULL || !verify->have_shared) {
        return NULL;
    }
    return verify->shared;
}
