/*
 * Host implementation of the two platform primitives, for the macOS test build.
 *
 * The device build uses PSA crypto (src/fp_platform_esp.c); this file provides
 * the same two entry points from CommonCrypto so the whole component - SAP hash,
 * white-box networks, message cipher, ekey wrapping - can be exercised with
 * clang before anything touches an ESP32.
 */
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>

#include <stddef.h>
#include <stdint.h>

#include "fp_internal.h"

int fp_sha512(const uint8_t *data, size_t len, uint8_t out[FP_SHA512_LEN])
{
    CC_SHA512_CTX ctx;

    if (out == NULL || (data == NULL && len != 0)) {
        return FP_ERR_ARG;
    }
    const uint8_t *input = (data != NULL) ? data : (const uint8_t *)"";
    if (CC_SHA512_Init(&ctx) != 1) {
        return FP_ERR_CRYPTO;
    }
    if (CC_SHA512_Update(&ctx, input, (CC_LONG)len) != 1) {
        return FP_ERR_CRYPTO;
    }
    return CC_SHA512_Final(out, &ctx) == 1 ? FP_OK : FP_ERR_CRYPTO;
}

int fp_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                 uint8_t out[FP_SHA1_LEN])
{
    CCHmacContext ctx;

    if (key == NULL || out == NULL || (msg == NULL && msg_len != 0)) {
        return FP_ERR_ARG;
    }
    const uint8_t *input = (msg != NULL) ? msg : (const uint8_t *)"";
    CCHmacInit(&ctx, kCCHmacAlgSHA1, key, key_len);
    CCHmacUpdate(&ctx, input, msg_len);
    CCHmacFinal(&ctx, out);
    return FP_OK;
}
