/*
 * Host-side crypto for the legacy AirTunes test: AES-128 and SHA-512 through
 * CommonCrypto, plus stand-ins for the HAP primitives that the legacy pair
 * code references but these tests do not exercise.
 */
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hap.h"

void ap_legacy_aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    size_t moved = 0;
    CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key, kCCKeySizeAES128, NULL, in, 16, out,
            16, &moved);
}

void hap_sha512(const uint8_t *in, size_t in_len, uint8_t out[64])
{
    CC_SHA512(in, (CC_LONG)in_len, out);
}

void hap_x25519_keypair(uint8_t public_key[32], uint8_t private_key[32])
{
    memset(public_key, 0xA5, 32);
    memset(private_key, 0x5A, 32);
}

int hap_x25519_secret(uint8_t shared[32], const uint8_t private_key[32], const uint8_t peer[32])
{
    memset(shared, 0x11, 32);
    (void)private_key;
    (void)peer;
    return 0;
}

void hap_identity_sign(const hap_identity_t *identity, const uint8_t *message, size_t message_len,
                       uint8_t signature[HAP_ED25519_SIG_LEN])
{
    (void)identity;
    (void)message;
    (void)message_len;
    memset(signature, 0x77, HAP_ED25519_SIG_LEN);
}

int hap_ed25519_verify(const uint8_t public_key[32], const uint8_t *message, size_t message_len,
                       const uint8_t signature[64])
{
    (void)public_key;
    (void)message;
    (void)message_len;
    (void)signature;
    return 0;
}


