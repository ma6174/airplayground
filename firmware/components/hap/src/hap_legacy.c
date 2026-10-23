/*
 * Primitives for the legacy AirTunes pairing (raw 32/68/96-byte exchanges,
 * no TLV8). The actual protocol sequencing lives in the airplay component;
 * this file only exposes what the vendored crypto already provides.
 */
#include "hap_internal.h"

#include <string.h>

#include "ed25519.h"
#include "psa/crypto.h"

void hap_x25519_keypair(uint8_t public_key[32], uint8_t private_key[32])
{
    randombytes(private_key, 32);
    hap_x25519_public_key(public_key, private_key);
}

int hap_x25519_secret(uint8_t shared[32], const uint8_t private_key[32], const uint8_t peer[32])
{
    return hap_x25519_shared(shared, private_key, peer);
}

int hap_ed25519_verify(const uint8_t public_key[32], const uint8_t *message, size_t message_len,
                       const uint8_t signature[64])
{
    return ed25519_verify(signature, message, message_len, public_key) == 1 ? 0 : -1;
}

void hap_sha512(const uint8_t *in, size_t in_len, uint8_t out[64])
{
    if (hap_psa_ensure_init() != 0) {
        memset(out, 0, 64);
        return;
    }

    size_t hash_len = 0;
    psa_hash_operation_t operation = psa_hash_operation_init();
    if (psa_hash_setup(&operation, PSA_ALG_SHA_512) != PSA_SUCCESS ||
        psa_hash_update(&operation, in, in_len) != PSA_SUCCESS ||
        psa_hash_finish(&operation, out, 64, &hash_len) != PSA_SUCCESS || hash_len != 64) {
        memset(out, 0, 64);
    }
}
