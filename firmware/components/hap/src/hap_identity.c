/*
 * The long-term controller identity: an Ed25519 key pair plus the pairing
 * identifier the receiver remembers. pair-setup M5 registers this identity and
 * pair-verify V3 proves possession of it, which is what lets later sessions skip
 * pair-setup entirely.
 */
#include "hap_internal.h"

#include <stdio.h>
#include <string.h>

#include "ed25519.h"

/* The vendored ed25519 keeps the 64-byte expanded private key: the clamped
 * scalar followed by the prefix SHA-512 produces from the seed. */
static void derive_keypair(const hap_identity_t *identity, uint8_t private_key[64],
                           uint8_t public_key[32])
{
    ed25519_create_keypair(public_key, private_key, identity->seed);
}

void hap_identity_generate(hap_identity_t *identity)
{
    uint8_t raw[16];
    uint8_t public_key[32];
    uint8_t private_key[64];

    if (identity == NULL) {
        return;
    }
    randombytes(identity->seed, sizeof identity->seed);
    derive_keypair(identity, private_key, public_key);
    memcpy(identity->public_key, public_key, sizeof identity->public_key);

    /* Upper-case version 4 UUID, the form receivers echo back as the pairing
     * identifier. */
    randombytes(raw, sizeof raw);
    raw[6] = (uint8_t)((raw[6] & 0x0FU) | 0x40U);
    raw[8] = (uint8_t)((raw[8] & 0x3FU) | 0x80U);
    snprintf(identity->pairing_id, sizeof identity->pairing_id,
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", raw[0],
             raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10],
             raw[11], raw[12], raw[13], raw[14], raw[15]);
}

int hap_identity_from_seed(hap_identity_t *identity, const uint8_t seed[HAP_ED25519_SEED_LEN],
                           const char *pairing_id)
{
    uint8_t public_key[32];
    uint8_t private_key[64];
    size_t length;

    if (identity == NULL || seed == NULL || pairing_id == NULL) {
        return -1;
    }
    length = strlen(pairing_id);
    if (length == 0 || length > HAP_ID_LEN) {
        HAP_LOGE("identity: pairing id must be 1..%d bytes", HAP_ID_LEN);
        return -1;
    }
    memcpy(identity->seed, seed, sizeof identity->seed);
    memcpy(identity->pairing_id, pairing_id, length);
    identity->pairing_id[length] = '\0';

    derive_keypair(identity, private_key, public_key);
    memcpy(identity->public_key, public_key, sizeof identity->public_key);
    return 0;
}

void hap_identity_sign(const hap_identity_t *identity, const uint8_t *message, size_t message_len,
                       uint8_t signature[HAP_ED25519_SIG_LEN])
{
    uint8_t private_key[64];
    uint8_t public_key[32];

    if (identity == NULL || signature == NULL) {
        return;
    }
    derive_keypair(identity, private_key, public_key);
    ed25519_sign(signature, message, message_len, public_key, private_key);
}
