/*
 * Host tests for the HAP component.
 *
 * The primitives (TLV8, big integers, SRP, Ed25519, X25519) are checked against
 * vectors from reference.py, and the PSA-backed paths (HKDF, ChaCha20-Poly1305)
 * run against the test-only stand-in in fake_psa.c so pair-setup, pair-verify
 * and the channel framing can be exercised end to end. For every exchange the
 * test plays the receiver's role with its own arithmetic, so a matching result
 * means both halves agree rather than one function agreeing with itself.
 *
 * The `syntax` target of the Makefile compiles the same sources against the real
 * mbedTLS 4.1 headers, which host builds cannot link.
 */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ed25519.h"
#include "hap_internal.h"
#include "sha512.h"
#include "vectors.h"

static int checks;
static int failures;

static void report(const char *what, int ok, const char *detail)
{
    fflush(stderr);
    checks++;
    if (ok) {
        printf("  ok    %s\n", what);
        fflush(stdout);
        return;
    }
    failures++;
    printf("  FAIL  %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static void check_bytes(const char *what, const uint8_t *actual, const uint8_t *expected,
                        size_t len)
{
    char detail[64] = {0};

    if (memcmp(actual, expected, len) != 0) {
        size_t index;

        for (index = 0; index < len; index++) {
            if (actual[index] != expected[index]) {
                snprintf(detail, sizeof detail, "first difference at byte %u (%02x != %02x)",
                         (unsigned)index, actual[index], expected[index]);
                break;
            }
        }
    }
    report(what, detail[0] == '\0', detail[0] ? detail : NULL);
}

static void check_true(const char *what, int condition)
{
    report(what, condition != 0, condition ? NULL : "condition is false");
}

/* ------------------------------------------------------------------ TLV8 */

static void test_tlv(void)
{
    /* M1 for transient pairing: method 0, state 1, flags 0x10 (little-endian). */
    static const uint8_t m1_expected[] = {0x00, 0x01, 0x00, 0x06, 0x01, 0x01,
                                          0x13, 0x04, 0x10, 0x00, 0x00, 0x00};
    uint8_t buf[HAP_TLV_MESSAGE_MAX];
    uint8_t big[HAP_SRP_WIDTH];
    uint8_t decoded[HAP_SRP_WIDTH];
    size_t decoded_len = 0;
    size_t index;
    hap_tlv_out_t message;

    printf("TLV8\n");

    hap_tlv_out_init(&message, buf, sizeof buf);
    hap_tlv_put_u8(&message, HAP_TLV_METHOD, 0x00);
    hap_tlv_put_u8(&message, HAP_TLV_STATE, 0x01);
    {
        const uint8_t flags[4] = {0x10, 0x00, 0x00, 0x00};
        hap_tlv_put(&message, HAP_TLV_FLAGS, flags, sizeof flags);
    }
    check_bytes("transient M1 encoding", buf, m1_expected, sizeof m1_expected);
    check_true("transient M1 length is 12", message.len == sizeof m1_expected);

    /* A 384-byte value must split into a 255-byte item and a 129-byte item. */
    for (index = 0; index < sizeof big; index++) {
        big[index] = (uint8_t)(index * 7 + 3);
    }
    hap_tlv_out_init(&message, buf, sizeof buf);
    hap_tlv_put(&message, HAP_TLV_PUBLIC_KEY, big, sizeof big);
    check_true("384-byte value becomes two segments",
               message.len == 2 + 255 + 2 + 129 && buf[0] == HAP_TLV_PUBLIC_KEY && buf[1] == 255 &&
                   buf[2 + 255] == HAP_TLV_PUBLIC_KEY && buf[2 + 255 + 1] == 129);

    check_true("decode joins segments",
               hap_tlv_get(buf, message.len, HAP_TLV_PUBLIC_KEY, decoded, sizeof decoded,
                           &decoded_len) == 0 &&
                   decoded_len == sizeof big);
    check_bytes("reassembled value matches", decoded, big, sizeof big);

    check_true("absent tag reports zero length",
               hap_tlv_get(buf, message.len, HAP_TLV_SALT, decoded, sizeof decoded, &decoded_len) ==
                       0 &&
                   decoded_len == 0);

    /* A value that does not fit the destination buffer is an error, not a
     * silent truncation. */
    check_true("insufficient capacity is rejected",
               hap_tlv_get(buf, message.len, HAP_TLV_PUBLIC_KEY, decoded, 128, &decoded_len) != 0);

    /* Truncated items must not be read past the end. */
    check_true("truncated message is rejected",
               hap_tlv_get(buf, 3, HAP_TLV_PUBLIC_KEY, decoded, sizeof decoded, &decoded_len) != 0);

    /* Overflow of the output buffer is detected. */
    hap_tlv_out_init(&message, buf, 8);
    check_true("output overflow is reported",
               hap_tlv_put(&message, HAP_TLV_PUBLIC_KEY, big, sizeof big) != 0 &&
                   message.overflow);

    {
        const uint8_t with_error[] = {0x06, 0x01, 0x02, 0x07, 0x01, 0x03};
        const uint8_t without_error[] = {0x06, 0x01, 0x02};
        check_true("error TLV is found", hap_tlv_error(with_error, sizeof with_error) == 3);
        check_true("missing error TLV reports -1",
                   hap_tlv_error(without_error, sizeof without_error) == -1);
    }
}

/* ------------------------------------------------------- big integers */

static void test_mpi(void)
{
    hap_mpi_ctx_t ctx;
    size_t index;

    printf("big integer\n");

    check_true("Montgomery context init", hap_mpi_ctx_init(&ctx, mpi_exp_one_mod,
                                                           sizeof mpi_exp_one_mod) == 0);
    check_true("n0inv matches Python", ctx.n0inv == mpi_n0inv);

    {
        /* The hex literal in hap_srp.c must parse to the same group the
         * reference derives, byte for byte. */
        uint8_t modulus[HAP_SRP_WIDTH];
        hap_srp_t srp;

        check_true("group context init",
                   hap_srp_client_start(&srp, "", srp_transient_salt, sizeof srp_transient_salt,
                                        srp_transient_B, sizeof srp_transient_B,
                                        srp_transient_a) == 0);
        check_true("group modulus export",
                   hap_mpi_to_bytes_padded(&srp.ctx.n, modulus, sizeof modulus) == sizeof modulus);
        check_bytes("group modulus matches Python", modulus, mpi_exp_one_mod,
                    sizeof mpi_exp_one_mod);
    }

    {
        uint8_t r2[HAP_MPI_BYTES];
        size_t len = hap_mpi_to_bytes_padded(&ctx.r2, r2, HAP_SRP_WIDTH);
        check_true("R^2 export width", len == HAP_SRP_WIDTH);
        check_bytes("R^2 mod N matches Python", r2, mpi_r2, sizeof mpi_r2);
    }

    for (index = 0; index < MPI_VECTOR_COUNT; index++) {
        const mpi_vector_t *vector = &mpi_vectors[index];
        hap_mpi_ctx_t local;
        hap_mpi_t base;
        hap_mpi_t result;
        uint8_t out[HAP_MPI_BYTES];
        size_t out_len;
        char label[96];

        snprintf(label, sizeof label, "modexp %s (%u-bit exponent)", vector->name,
                 (unsigned)vector->exp_len * 8);
        if (hap_mpi_ctx_init(&local, vector->mod, vector->mod_len) != 0 ||
            hap_mpi_from_bytes(&base, vector->base, vector->base_len) != 0 ||
            hap_mpi_modexp(&local, &result, &base, vector->exp, vector->exp_len) != 0) {
            report(label, 0, "computation failed");
            continue;
        }
        out_len = hap_mpi_to_bytes(&result, out, sizeof out);
        if (out_len != vector->expected_len) {
            report(label, 0, "result length differs");
            continue;
        }
        check_bytes(label, out, vector->expected, vector->expected_len);
    }

    /* Rejections: even modulus and an unreduced base. */
    {
        const uint8_t even_mod[4] = {0x00, 0x00, 0x00, 0x08};
        check_true("even modulus is rejected", hap_mpi_ctx_init(&ctx, even_mod, sizeof even_mod) != 0);
    }
}

/* ---------------------------------------------------------------- SRP-6a */

static void run_srp_case(const char *name, const char *pin, const uint8_t *salt, size_t salt_len,
                         const uint8_t *server_public, size_t server_public_len, const uint8_t *client_a,
                         const uint8_t *expected_a, const uint8_t *expected_k,
                         const uint8_t *expected_m1, const uint8_t *expected_m2)
{
    hap_srp_t srp;
    char label[96];
    uint8_t tampered[HAP_SRP_HASH_LEN];

    snprintf(label, sizeof label, "%s: SRP exchange", name);
    if (hap_srp_client_start(&srp, pin, salt, salt_len, server_public, server_public_len,
                              client_a) != 0) {
        report(label, 0, "hap_srp_client_start failed");
        return;
    }
    report(label, 1, NULL);

    snprintf(label, sizeof label, "%s: client public key A", name);
    check_bytes(label, srp.padded_a, expected_a, HAP_SRP_WIDTH);
    snprintf(label, sizeof label, "%s: session key K", name);
    check_bytes(label, srp.k, expected_k, HAP_SRP_HASH_LEN);
    snprintf(label, sizeof label, "%s: client proof M1", name);
    check_bytes(label, srp.m1, expected_m1, HAP_SRP_HASH_LEN);
    snprintf(label, sizeof label, "%s: expected server proof M2", name);
    check_bytes(label, srp.m2, expected_m2, HAP_SRP_HASH_LEN);

    snprintf(label, sizeof label, "%s: server proof accepted", name);
    check_true(label, hap_srp_verify_server_proof(&srp, expected_m2, HAP_SRP_HASH_LEN) == 0);

    memcpy(tampered, expected_m2, sizeof tampered);
    tampered[0] ^= 0x01;
    snprintf(label, sizeof label, "%s: corrupted server proof rejected", name);
    check_true(label, hap_srp_verify_server_proof(&srp, tampered, HAP_SRP_HASH_LEN) != 0);

    snprintf(label, sizeof label, "%s: short server proof rejected", name);
    check_true(label, hap_srp_verify_server_proof(&srp, expected_m2, 32) != 0);
}

static void test_srp(void)
{
    printf("SRP-6a\n");

    run_srp_case("transient", "", srp_transient_salt, sizeof srp_transient_salt, srp_transient_B,
                 sizeof srp_transient_B, srp_transient_a, srp_transient_pad_A, srp_transient_K,
                 srp_transient_M1, srp_transient_M2);

    run_srp_case("pin", "1234", srp_pin_salt, sizeof srp_pin_salt, srp_pin_B, sizeof srp_pin_B,
                 srp_pin_a, srp_pin_pad_A, srp_pin_K, srp_pin_M1, srp_pin_M2);

    /* Out-of-range server public keys must be refused before any exponentiation. */
    {
        hap_srp_t srp;
        uint8_t zero_b[HAP_SRP_WIDTH] = {0};
        uint8_t out_of_range[HAP_SRP_WIDTH];

        memset(out_of_range, 0xFF, sizeof out_of_range);
        check_true("B = 0 is rejected",
                   hap_srp_client_start(&srp, "", srp_transient_salt, sizeof srp_transient_salt,
                                        zero_b, sizeof zero_b, srp_transient_a) != 0);
        check_true("B >= N is rejected",
                   hap_srp_client_start(&srp, "", srp_transient_salt, sizeof srp_transient_salt,
                                        out_of_range, sizeof out_of_range,
                                        srp_transient_a) != 0);
    }
}

/* -------------------------------------------------------------- Ed25519 */

static void test_ed25519(void)
{
    /* RFC 8032 section 7.1, test 1 and test 2. */
    static const uint8_t seed1[32] = {0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
                                      0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
                                      0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
                                      0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};
    static const uint8_t pub1[32] = {0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7,
                                     0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
                                     0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25,
                                     0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
    static const uint8_t sig1[64] = {0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72,
                                     0x90, 0x86, 0xe2, 0xcc, 0x80, 0x6e, 0x82, 0x8a,
                                     0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74,
                                     0xd8, 0x73, 0xe0, 0x65, 0x22, 0x49, 0x01, 0x55,
                                     0x5f, 0xb8, 0x82, 0x15, 0x90, 0xa3, 0x3b, 0xac,
                                     0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9, 0xb4, 0x6b,
                                     0xd2, 0x5b, 0xf5, 0xf0, 0x59, 0x5b, 0xbe, 0x24,
                                     0x65, 0x51, 0x41, 0x43, 0x8e, 0x7a, 0x10, 0x0b};
    static const uint8_t seed2[32] = {0x4c, 0xcd, 0x08, 0x9b, 0x28, 0xff, 0x96, 0xda,
                                      0x9d, 0xb6, 0xc3, 0x46, 0xec, 0x11, 0x4e, 0x0f,
                                      0x5b, 0x8a, 0x31, 0x9f, 0x35, 0xab, 0xa6, 0x24,
                                      0xda, 0x8c, 0xf6, 0xed, 0x4f, 0xb8, 0xa6, 0xfb};
    static const uint8_t pub2[32] = {0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a,
                                     0x92, 0xb7, 0x0a, 0xa7, 0x4d, 0x1b, 0x7e, 0xbc,
                                     0x9c, 0x98, 0x2c, 0xcf, 0x2e, 0xc4, 0x96, 0x8c,
                                     0xc0, 0xcd, 0x55, 0xf1, 0x2a, 0xf4, 0x66, 0x0c};
    static const uint8_t message2[] = {0x72};
    static const uint8_t sig2[64] = {0x92, 0xa0, 0x09, 0xa9, 0xf0, 0xd4, 0xca, 0xb8,
                                     0x72, 0x0e, 0x82, 0x0b, 0x5f, 0x64, 0x25, 0x40,
                                     0xa2, 0xb2, 0x7b, 0x54, 0x16, 0x50, 0x3f, 0x8f,
                                     0xb3, 0x76, 0x22, 0x23, 0xeb, 0xdb, 0x69, 0xda,
                                     0x08, 0x5a, 0xc1, 0xe4, 0x3e, 0x15, 0x99, 0x6e,
                                     0x45, 0x8f, 0x36, 0x13, 0xd0, 0xf1, 0x1d, 0x8c,
                                     0x38, 0x7b, 0x2e, 0xae, 0xb4, 0x30, 0x2a, 0xee,
                                     0xb0, 0x0d, 0x29, 0x16, 0x12, 0xbb, 0x0c, 0x00};
    hap_identity_t identity;
    uint8_t signature[HAP_ED25519_SIG_LEN];

    printf("Ed25519 (RFC 8032)\n");

    check_true("identity from seed 1", hap_identity_from_seed(&identity, seed1,
                                                              "5A1B2C3D-0000-4000-8000-000000000001") == 0);
    check_bytes("public key 1", identity.public_key, pub1, sizeof pub1);
    hap_identity_sign(&identity, (const uint8_t *)"", 0, signature);
    check_bytes("signature 1", signature, sig1, sizeof sig1);
    check_true("signature 1 verifies", ed25519_verify(sig1, (const uint8_t *)"", 0, pub1) == 1);

    check_true("identity from seed 2", hap_identity_from_seed(&identity, seed2,
                                                              "5A1B2C3D-0000-4000-8000-000000000002") == 0);
    check_bytes("public key 2", identity.public_key, pub2, sizeof pub2);
    hap_identity_sign(&identity, message2, sizeof message2, signature);
    check_bytes("signature 2", signature, sig2, sizeof sig2);
    check_true("signature 2 verifies",
               ed25519_verify(sig2, message2, sizeof message2, pub2) == 1);

    signature[0] ^= 0x01;
    check_true("corrupted signature is rejected",
               ed25519_verify(signature, message2, sizeof message2, pub2) != 1);

    /* Generated identities are usable as-is. */
    hap_identity_generate(&identity);
    hap_identity_sign(&identity, message2, sizeof message2, signature);
    check_true("generated identity signs verifiably",
               ed25519_verify(signature, message2, sizeof message2, identity.public_key) == 1);
    check_true("generated pairing id looks like an upper-case v4 UUID",
               strlen(identity.pairing_id) == 36 && identity.pairing_id[8] == '-' &&
                   identity.pairing_id[13] == '-' && identity.pairing_id[14] == '4' &&
                   identity.pairing_id[18] == '-' && identity.pairing_id[23] == '-');
}

/* --------------------------------------------------------------- X25519 */

static void test_x25519(void)
{
    /* RFC 7748 section 6.1. */
    static const uint8_t alice_private[32] = {0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
                                              0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
                                              0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
                                              0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a};
    static const uint8_t alice_public[32] = {0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54,
                                             0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a,
                                             0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4,
                                             0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a};
    static const uint8_t bob_private[32] = {0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b,
                                            0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
                                            0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd,
                                            0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb};
    static const uint8_t bob_public[32] = {0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4,
                                           0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
                                           0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d,
                                           0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f};
    static const uint8_t shared_secret[32] = {0x4a, 0x5d, 0x9d, 0x5b, 0xa4, 0xce, 0x2d, 0xe1,
                                              0x72, 0x8e, 0x3b, 0xf4, 0x80, 0x35, 0x0f, 0x25,
                                              0xe0, 0x7e, 0x21, 0xc9, 0x47, 0xd1, 0x9e, 0x33,
                                              0x76, 0xf0, 0x9b, 0x3c, 0x1e, 0x16, 0x17, 0x42};
    /* RFC 7748 section 5.2, first test vector. */
    static const uint8_t scalar[32] = {0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d,
                                       0x3b, 0x16, 0x15, 0x4b, 0x82, 0x46, 0x5e, 0xdd,
                                       0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc, 0x5a, 0x18,
                                       0x50, 0x6a, 0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4};
    static const uint8_t u_in[32] = {0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb,
                                     0x35, 0x94, 0xc1, 0xa4, 0x24, 0xb1, 0x5f, 0x7c,
                                     0x72, 0x66, 0x24, 0xec, 0x26, 0xb3, 0x35, 0x3b,
                                     0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c};
    static const uint8_t u_out[32] = {0xc3, 0xda, 0x55, 0x37, 0x9d, 0xe9, 0xc6, 0x90,
                                      0x8e, 0x94, 0xea, 0x4d, 0xf2, 0x8d, 0x08, 0x4f,
                                      0x32, 0xec, 0xcf, 0x03, 0x49, 0x1c, 0x71, 0xf7,
                                      0x54, 0xb4, 0x07, 0x55, 0x77, 0xa2, 0x85, 0x52};
    uint8_t public_key[32];
    uint8_t shared[32];

    printf("X25519 (RFC 7748)\n");

    hap_x25519_public_key(public_key, alice_private);
    check_bytes("Alice public key", public_key, alice_public, sizeof alice_public);
    hap_x25519_public_key(public_key, bob_private);
    check_bytes("Bob public key", public_key, bob_public, sizeof bob_public);

    check_true("shared secret established",
               hap_x25519_shared(shared, alice_private, bob_public) == 0);
    check_bytes("Alice side shared secret", shared, shared_secret, sizeof shared_secret);
    check_true("shared secret established",
               hap_x25519_shared(shared, bob_private, alice_public) == 0);
    check_bytes("Bob side shared secret", shared, shared_secret, sizeof shared_secret);

    check_true("arbitrary scalar/u vector", hap_x25519_shared(shared, scalar, u_in) == 0);
    check_bytes("arbitrary scalar/u output", shared, u_out, sizeof u_out);

    {
        uint8_t low_order[32] = {0};
        check_true("all-zero peer key is rejected", hap_x25519_shared(shared, alice_private, low_order) != 0);
    }
}

/* ------------------------------------------------------- PSA stand-ins */

static void test_aead(void)
{
    uint8_t sealed[sizeof aead_frame_plain + 16];
    uint8_t opened[sizeof aead_frame_plain];
    uint8_t label_sealed[sizeof aead_label_plain + 16];

    printf("AEAD (host PSA stand-in vs reference.py)\n");

    check_true("counter-nonce seal",
               hap_aead_seal(aead_frame_key, aead_frame_nonce, aead_frame_aad,
                             sizeof aead_frame_aad, aead_frame_plain,
                             sizeof aead_frame_plain, sealed) == 0);
    check_bytes("counter-nonce ciphertext+tag", sealed, aead_frame_sealed,
                sizeof aead_frame_sealed);
    check_true("counter-nonce open",
               hap_aead_open(aead_frame_key, aead_frame_nonce, aead_frame_aad,
                             sizeof aead_frame_aad, sealed, sizeof sealed, opened) == 0);
    check_bytes("round-tripped plaintext", opened, aead_frame_plain, sizeof aead_frame_plain);

    sealed[0] ^= 0x01;
    check_true("tampered ciphertext is rejected",
               hap_aead_open(aead_frame_key, aead_frame_nonce, aead_frame_aad,
                             sizeof aead_frame_aad, sealed, sizeof sealed, opened) != 0);

    check_true("label-nonce seal",
               hap_aead_seal_label(aead_label_key, "PS-Msg05", NULL, 0, aead_label_plain,
                                   sizeof aead_label_plain, label_sealed) == 0);
    check_bytes("label-nonce ciphertext+tag", label_sealed, aead_label_sealed,
                sizeof aead_label_sealed);
}

static void test_kdf(void)
{
    uint8_t hkdf[32];
    uint8_t write_key[32];
    uint8_t read_key[32];
    uint8_t data_key[32];

    printf("HKDF-SHA512 (host PSA stand-in vs reference.py)\n");

    check_true("raw HKDF", hap_hkdf_sha512(hkdf_ikm, sizeof hkdf_ikm, HAP_HKDF_SALT_CONTROL,
                                           HAP_HKDF_INFO_CONTROL_WRITE, hkdf, sizeof hkdf) == 0);
    check_bytes("HKDF output", hkdf, hkdf_expected, sizeof hkdf_expected);

    hap_derive_control_keys(channel_shared, write_key, read_key);
    check_bytes("control write key", write_key, channel_control_write,
                sizeof channel_control_write);
    check_bytes("control read key", read_key, channel_control_read, sizeof channel_control_read);

    hap_derive_event_keys(channel_shared, write_key, read_key);
    check_bytes("event write key (reversed direction)", write_key, channel_event_write,
                sizeof channel_event_write);
    check_bytes("event read key (reversed direction)", read_key, channel_event_read,
                sizeof channel_event_read);

    check_true("data stream key",
               hap_derive_data_key(channel_shared, sizeof channel_shared, channel_stream_id,
                                   data_key) == 0);
    check_bytes("data stream key bytes", data_key, channel_data_key, sizeof channel_data_key);
}

/* ----------------------------------------------------------- channel */

static void test_channel(void)
{
    static const char first_chunk[] = "RTSP/1.0 200 OK\r\nCSeq: 1\r\n";
    uint8_t payload[2500];
    uint8_t wire[4096];
    uint8_t recovered[2500];
    uint8_t write_key[32];
    uint8_t read_key[32];
    hap_channel_t writer;
    hap_channel_t reader;
    int pair[2];
    ssize_t received;
    size_t index;

    printf("HAP framing over a socket pair\n");

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        report("socket pair", 0, "socketpair failed");
        return;
    }
    for (index = 0; index < sizeof payload; index++) {
        payload[index] = (uint8_t)(index * 13 + 1);
    }
    hap_derive_control_keys(channel_shared, write_key, read_key);

    hap_channel_init(&writer, pair[0]);
    hap_channel_init(&reader, pair[1]);

    check_true("plaintext write",
               hap_channel_write(&writer, first_chunk, sizeof first_chunk - 1) == 0);
    {
        char line[sizeof first_chunk];
        check_true("plaintext read",
                   hap_channel_read(&reader, line, sizeof first_chunk - 1) == 0 &&
                       memcmp(line, first_chunk, sizeof first_chunk - 1) == 0);
    }

    hap_channel_enable_encryption(&writer, write_key, read_key);
    hap_channel_enable_encryption(&reader, read_key, write_key);

    check_true("2500-byte write splits into frames",
               hap_channel_write(&writer, payload, sizeof payload) == 0);
    check_true("2500-byte read joins frames",
               hap_channel_read(&reader, recovered, sizeof recovered) == 0);
    check_bytes("round-tripped payload", recovered, payload, sizeof payload);
    check_true("write counter advanced by three frames", writer.write_nonce == 3);
    check_true("read counter advanced by three frames", reader.read_nonce == 3);

    /* Inspect the wire format of one frame: the length prefix is the AAD and is
     * therefore not encrypted, so it must show up verbatim. */
    check_true("frame write", hap_channel_write(&writer, payload, 100) == 0);
    received = read(pair[1], wire, sizeof wire);
    check_true("one 118-byte frame on the wire", received == 2 + 100 + 16);
    check_true("length prefix is little-endian plaintext length",
               received == 118 && wire[0] == 100 && wire[1] == 0);
    {
        uint8_t decrypted[100];
        check_true("frame decrypts with AAD = prefix",
                   hap_aead_open(write_key, writer.write_nonce - 1, wire, 2, wire + 2, 116,
                                 decrypted) == 0);
        check_bytes("frame content", decrypted, payload, sizeof decrypted);
    }

    close(pair[0]);
    close(pair[1]);
}

/* ------------------------------------------- pair-setup, receiver side */

/* Plays the receiver's SRP role so the client's M3/M4/M5 can be checked without
 * reusing the code under test. */
typedef struct {
    uint8_t salt[16];
    uint8_t secret_b[32];
    hap_mpi_ctx_t ctx;
    hap_mpi_t b;
    hap_mpi_t verification;
    uint8_t public_b[HAP_SRP_WIDTH];
    size_t public_b_len;
} srp_server_t;

static void srp_server_init(srp_server_t *server, const char *pin, const uint8_t *salt,
                            const uint8_t *secret_b)
{
    uint8_t padded_g[HAP_SRP_WIDTH] = {0};
    uint8_t x_hash[64];
    uint8_t inner[64];
    uint8_t k_hash[64];
    sha512_context hash;
    hap_mpi_t generator;
    hap_mpi_t k;
    hap_mpi_t scaled;
    hap_mpi_t public_b;
    hap_mpi_t limit;

    memcpy(server->salt, salt, sizeof server->salt);
    memcpy(server->secret_b, secret_b, sizeof server->secret_b);
    hap_mpi_ctx_init(&server->ctx, mpi_exp_one_mod, sizeof mpi_exp_one_mod);

    memset(&generator, 0, sizeof generator);
    generator.d[0] = 5;
    padded_g[HAP_SRP_WIDTH - 1] = 5;

    /* x = H(salt || H("Pair-Setup:" || PIN)) */
    sha512_init(&hash);
    sha512_update(&hash, (const unsigned char *)"Pair-Setup", 10);
    sha512_update(&hash, (const unsigned char *)":", 1);
    sha512_update(&hash, (const unsigned char *)pin, strlen(pin));
    sha512_final(&hash, inner);
    sha512_init(&hash);
    sha512_update(&hash, server->salt, sizeof server->salt);
    sha512_update(&hash, inner, sizeof inner);
    sha512_final(&hash, x_hash);

    /* k = H(pad(N) || pad(g)) */
    sha512_init(&hash);
    sha512_update(&hash, mpi_exp_one_mod, sizeof mpi_exp_one_mod);
    sha512_update(&hash, padded_g, sizeof padded_g);
    sha512_final(&hash, k_hash);

    hap_mpi_from_bytes(&k, k_hash, sizeof k_hash);
    hap_mpi_from_bytes(&server->b, server->secret_b, sizeof server->secret_b);

    /* v = g^x stays around for the verification step. */
    hap_mpi_modexp(&server->ctx, &server->verification, &generator, x_hash, sizeof x_hash);
    hap_mpi_mulmod(&server->ctx, &scaled, &k, &server->verification); /* k*v */

    /* B = (k*v + g^b) mod N. Two reduced values can sum to one limb more than N,
     * so reduce by subtracting N - k*v when g^b is large enough. */
    hap_mpi_modexp(&server->ctx, &public_b, &generator, server->secret_b,
                   sizeof server->secret_b);
    hap_mpi_sub(&limit, &server->ctx.n, &scaled);
    if (hap_mpi_cmp(&public_b, &limit) >= 0) {
        hap_mpi_sub(&public_b, &public_b, &limit);
    } else {
        hap_mpi_add(&public_b, &public_b, &scaled);
    }
    hap_mpi_to_bytes_padded(&public_b, server->public_b, HAP_SRP_WIDTH);
    server->public_b_len = HAP_SRP_WIDTH;
}

/* Returns K, the client's SRP session hash. */
static int srp_server_finish(srp_server_t *server, const uint8_t *client_a, size_t client_a_len,
                             const uint8_t *proof, size_t proof_len, uint8_t session_key[64],
                             uint8_t server_proof[64])
{
    uint8_t padded_a[HAP_SRP_WIDTH] = {0};
    uint8_t u_hash[64];
    uint8_t s_bytes[HAP_SRP_WIDTH];
    uint8_t inner[64];
    uint8_t hxor[64];
    uint8_t generator_hash[64];
    uint8_t user_hash[64];
    uint8_t expected_proof[64];
    size_t a_len = client_a_len;
    size_t s_len;
    hap_mpi_t a;
    hap_mpi_t u;
    hap_mpi_t vu;
    hap_mpi_t secret;
    sha512_context hash;
    const uint8_t *a_min;
    const uint8_t *b_min;
    size_t b_len = 0;

    if (a_len > HAP_SRP_WIDTH) {
        return -1;
    }
    memcpy(padded_a + (HAP_SRP_WIDTH - a_len), client_a, a_len);

    /* SRP hashes A and B at their natural widths; the wire form is padded. */
    while (a_len > 0 && padded_a[HAP_SRP_WIDTH - a_len] == 0) {
        a_len--;
    }
    a_min = padded_a + (HAP_SRP_WIDTH - a_len);
    while (b_len < sizeof server->public_b && server->public_b[b_len] == 0) {
        b_len++;
    }
    b_len = sizeof server->public_b - b_len;
    b_min = server->public_b + (sizeof server->public_b - b_len);

    sha512_init(&hash);
    sha512_update(&hash, padded_a, sizeof padded_a);
    sha512_update(&hash, server->public_b, sizeof server->public_b);
    sha512_final(&hash, u_hash);

    hap_mpi_from_bytes(&a, padded_a, sizeof padded_a);
    hap_mpi_from_bytes(&u, u_hash, sizeof u_hash);
    /* S = (A * v^u)^b mod N */
    hap_mpi_modexp(&server->ctx, &vu, &server->verification, u_hash, sizeof u_hash);
    hap_mpi_mulmod(&server->ctx, &vu, &a, &vu);
    hap_mpi_modexp(&server->ctx, &secret, &vu, server->secret_b, sizeof server->secret_b);
    s_len = hap_mpi_to_bytes(&secret, s_bytes, sizeof s_bytes);
    if (s_len == (size_t)-1) {
        return -1;
    }

    sha512_init(&hash);
    sha512_update(&hash, s_bytes, s_len);
    sha512_final(&hash, session_key);

    sha512_init(&hash);
    sha512_update(&hash, mpi_exp_one_mod, sizeof mpi_exp_one_mod);
    sha512_final(&hash, inner);
    sha512_init(&hash);
    sha512_update(&hash, (const unsigned char *)"\x05", 1);
    sha512_final(&hash, generator_hash);
    for (size_t index = 0; index < 64; index++) {
        hxor[index] = (uint8_t)(inner[index] ^ generator_hash[index]);
    }
    sha512_init(&hash);
    sha512_update(&hash, (const unsigned char *)"Pair-Setup", 10);
    sha512_final(&hash, user_hash);

    sha512_init(&hash);
    sha512_update(&hash, hxor, sizeof hxor);
    sha512_update(&hash, user_hash, sizeof user_hash);
    sha512_update(&hash, server->salt, sizeof server->salt);
    sha512_update(&hash, a_min, a_len);
    sha512_update(&hash, b_min, b_len);
    sha512_update(&hash, session_key, 64);
    sha512_final(&hash, expected_proof);

    if (proof_len != sizeof expected_proof || memcmp(proof, expected_proof, proof_len) != 0) {
        HAP_LOGE("test: client M1 proof mismatch");
        return -1;
    }

    /* M4: the receiver echoes H(A || M1 || K), A again at its natural width. */
    sha512_init(&hash);
    sha512_update(&hash, a_min, a_len);
    sha512_update(&hash, proof, proof_len);
    sha512_update(&hash, session_key, 64);
    sha512_final(&hash, server_proof);
    return 0;
}

static void run_setup_case(const char *name, const char *pin, const uint8_t *salt,
                           const uint8_t *secret_b, bool expect_acl)
{
    uint8_t message[HAP_TLV_MESSAGE_MAX];
    uint8_t session_key[32];
    uint8_t sign_key[32];
    uint8_t srv_session_key[64];
    uint8_t server_proof[64];
    uint8_t client_a[HAP_SRP_WIDTH];
    uint8_t client_proof[64];
    uint8_t plain[256];
    uint8_t sign_input[32 + HAP_ID_LEN + 32];
    uint8_t identifier[HAP_ID_LEN];
    size_t len = 0;
    size_t client_a_len = 0;
    size_t client_proof_len = 0;
    size_t plain_len = 0;
    size_t identifier_len = 0;
    hap_identity_t identity;
    hap_setup_t *setup;
    srp_server_t server;
    char label[96];
    uint8_t sub[256];
    hap_tlv_out_t out;

    snprintf(label, sizeof label, "%s: pair-setup completes", name);
    hap_identity_generate(&identity);
    srp_server_init(&server, pin, salt, secret_b);
    setup = hap_setup_new(&identity, pin, expect_acl);
    if (setup == NULL) {
        report(label, 0, "hap_setup_new failed");
        return;
    }

    /* M1 -> M2 */
    if (hap_setup_begin(setup, message, sizeof message, &len) != 0 ||
        len != (pin[0] == '\0' ? 12u : 6u)) {
        report(label, 0, "M1 failed");
        hap_setup_free(setup);
        return;
    }
    hap_tlv_out_init(&out, message, sizeof message);
    hap_tlv_put_u8(&out, HAP_TLV_STATE, 0x02);
    hap_tlv_put(&out, HAP_TLV_SALT, server.salt, sizeof server.salt);
    hap_tlv_put(&out, HAP_TLV_PUBLIC_KEY, server.public_b, server.public_b_len);
    {
        uint8_t m3[512];
        size_t m2_len = out.len;
        size_t m3_len = 0;

        if (hap_setup_m2(setup, message, m2_len, m3, sizeof m3, &m3_len) != 0) {
            report(label, 0, "M3 failed");
            hap_setup_free(setup);
            return;
        }
        /* M3 = state 3, padded A, M1 proof */
        if (hap_tlv_get(m3, m3_len, HAP_TLV_PUBLIC_KEY, client_a, sizeof client_a,
                        &client_a_len) != 0 ||
            hap_tlv_get(m3, m3_len, HAP_TLV_PROOF, client_proof, sizeof client_proof,
                        &client_proof_len) != 0) {
            report(label, 0, "M3 is malformed");
            hap_setup_free(setup);
            return;
        }
        report(label, 1, NULL);
    }

    snprintf(label, sizeof label, "%s: client A is 384 bytes", name);
    check_true(label, client_a_len == HAP_SRP_WIDTH);
    snprintf(label, sizeof label, "%s: client proof is 64 bytes", name);
    check_true(label, client_proof_len == 64);

    snprintf(label, sizeof label, "%s: receiver accepts the client proof", name);
    check_true(label, srp_server_finish(&server, client_a, client_a_len, client_proof,
                                        client_proof_len, srv_session_key, server_proof) == 0);

    /* M4 -> M5 */
    {
        uint8_t m4[128];
        uint8_t m5[512];
        size_t m4_len;
        size_t m5_len = 0;
        size_t sealed_len = 0;

        hap_tlv_out_init(&out, m4, sizeof m4);
        hap_tlv_put_u8(&out, HAP_TLV_STATE, 0x04);
        hap_tlv_put(&out, HAP_TLV_PROOF, server_proof, sizeof server_proof);
        m4_len = out.len;

        snprintf(label, sizeof label, "%s: M5 produced", name);
        if (hap_setup_m4(setup, m4, m4_len, m5, sizeof m5, &m5_len) != 0) {
            report(label, 0, "M5 failed");
            hap_setup_free(setup);
            return;
        }
        report(label, 1, NULL);

        /* The receiver derives the same session key from K and opens M5. */
        check_true("receiver HKDF matches",
                   hap_hkdf_sha512(srv_session_key, sizeof srv_session_key,
                                   HAP_HKDF_SALT_SETUP_ENCRYPT, HAP_HKDF_INFO_SETUP_ENCRYPT,
                                   session_key, sizeof session_key) == 0 &&
                       hap_hkdf_sha512(srv_session_key, sizeof srv_session_key,
                                       HAP_HKDF_SALT_SETUP_SIGN, HAP_HKDF_INFO_SETUP_SIGN,
                                       sign_key, sizeof sign_key) == 0);

        snprintf(label, sizeof label, "%s: M5 carries encrypted data", name);
        check_true(label, hap_tlv_get(m5, m5_len, HAP_TLV_ENCRYPTED_DATA, sub, sizeof sub,
                                      &sealed_len) == 0 &&
                              sealed_len > 16);
        snprintf(label, sizeof label, "%s: M5 payload opens with the session key", name);
        check_true(label, hap_aead_open_label(session_key, "PS-Msg05", NULL, 0, sub, sealed_len,
                                             plain) == 0);
        plain_len = sealed_len - 16;
    }

    snprintf(label, sizeof label, "%s: sub-TLV identifiers the client", name);
    check_true(label, hap_tlv_get(plain, plain_len, HAP_TLV_IDENTIFIER, identifier,
                                 sizeof identifier, &identifier_len) == 0 &&
                         identifier_len == strlen(identity.pairing_id) &&
                         memcmp(identifier, identity.pairing_id, identifier_len) == 0);

    snprintf(label, sizeof label, "%s: sub-TLV carries the Ed25519 key", name);
    {
        uint8_t public_key[32];
        size_t public_len = 0;

        check_true(label, hap_tlv_get(plain, plain_len, HAP_TLV_PUBLIC_KEY, public_key,
                                     sizeof public_key, &public_len) == 0 &&
                             public_len == 32 &&
                             memcmp(public_key, identity.public_key, 32) == 0);
    }

    snprintf(label, sizeof label, "%s: signature covers sigKey||id||publicKey", name);
    {
        uint8_t signature[64];
        size_t signature_len = 0;
        size_t offset = 0;

        memcpy(sign_input + offset, sign_key, sizeof sign_key);
        offset += sizeof sign_key;
        memcpy(sign_input + offset, identity.pairing_id, strlen(identity.pairing_id));
        offset += strlen(identity.pairing_id);
        memcpy(sign_input + offset, identity.public_key, 32);
        offset += 32;

        check_true(label, hap_tlv_get(plain, plain_len, HAP_TLV_SIGNATURE, signature,
                                     sizeof signature, &signature_len) == 0 &&
                             signature_len == 64 &&
                             ed25519_verify(signature, sign_input, offset,
                                            identity.public_key) == 1);
    }

    snprintf(label, sizeof label, "%s: ACL %s", name, expect_acl ? "present" : "absent");
    {
        uint8_t acl[32];
        size_t acl_len = 0;
        int present = hap_tlv_get(plain, plain_len, HAP_TLV_ACL, acl, sizeof acl, &acl_len) == 0 &&
                      acl_len != 0;

        check_true(label, present == expect_acl &&
                             (!present || (acl_len == HAP_ACL_SCREEN_CAPTURE_LEN &&
                                           memcmp(acl, HAP_ACL_SCREEN_CAPTURE, acl_len) == 0)));
    }

    snprintf(label, sizeof label, "%s: M6 completes the exchange", name);
    {
        uint8_t m6[8];
        hap_tlv_out_t ack;

        hap_tlv_out_init(&ack, m6, sizeof m6);
        hap_tlv_put_u8(&ack, HAP_TLV_STATE, 0x06);
        check_true(label, hap_setup_m6(setup, m6, ack.len) == 0);
    }

    hap_setup_free(setup);
}

static void test_setup(void)
{
    printf("pair-setup (receiver role played by the test)\n");

    run_setup_case("transient", "", srp_transient_salt, srp_transient_secret_b, false);
    run_setup_case("pin", "1234", srp_pin_salt, srp_pin_secret_b, true);

    /* A wrong server proof must abort the exchange. */
    {
        uint8_t message[512];
        uint8_t scratch[512];
        size_t len = 0;
        srp_server_t server;
        hap_identity_t identity;
        hap_setup_t *setup;

        hap_identity_generate(&identity);
        setup = hap_setup_new(&identity, "", false);
        srp_server_init(&server, "", srp_transient_salt, srp_transient_secret_b);
        hap_setup_begin(setup, message, sizeof message, &len);
        {
            hap_tlv_out_t m2;

            hap_tlv_out_init(&m2, scratch, sizeof scratch);
            hap_tlv_put_u8(&m2, HAP_TLV_STATE, 0x02);
            hap_tlv_put(&m2, HAP_TLV_SALT, server.salt, sizeof server.salt);
            hap_tlv_put(&m2, HAP_TLV_PUBLIC_KEY, server.public_b, server.public_b_len);
            check_true("valid M2 is accepted",
                       hap_setup_m2(setup, scratch, m2.len, message, sizeof message, &len) == 0);
        }
        {
            hap_tlv_out_t out;
            uint8_t bad[80];
            uint8_t proof[64] = {0};

            hap_tlv_out_init(&out, bad, sizeof bad);
            hap_tlv_put_u8(&out, HAP_TLV_STATE, 0x04);
            hap_tlv_put(&out, HAP_TLV_PROOF, proof, sizeof proof);
            check_true("bad server proof is rejected",
                       hap_setup_m4(setup, bad, out.len, scratch, sizeof scratch, &len) != 0);
        }
        hap_setup_free(setup);
    }
}

/* ------------------------------------------ pair-verify, receiver side */

static void test_verify(void)
{
    uint8_t message[512];
    uint8_t v2[512];
    uint8_t v3[512];
    uint8_t plain[256];
    uint8_t verify_key[32];
    uint8_t shared_by_client[32];
    uint8_t shared_by_receiver[32];
    uint8_t receiver_private[32];
    uint8_t receiver_public[32];
    uint8_t client_public[32];
    uint8_t signature[64];
    uint8_t identifier[HAP_ID_LEN];
    uint8_t sign_input[32 + HAP_ID_LEN + 32];
    size_t len = 0;
    size_t sealed_len = 0;
    size_t identifier_len = 0;
    size_t signature_len = 0;
    size_t offset = 0;
    hap_identity_t identity;
    hap_verify_t *verify;

    printf("pair-verify (receiver role played by the test)\n");

    hap_identity_generate(&identity);
    verify = hap_verify_new(&identity);
    randombytes(receiver_private, sizeof receiver_private);
    hap_x25519_public_key(receiver_public, receiver_private);

    check_true("V1 produced",
               hap_verify_begin(verify, message, sizeof message, &len) == 0);
    check_true("V1 carries the client ephemeral key",
               hap_tlv_get(message, len, HAP_TLV_PUBLIC_KEY, client_public,
                           sizeof client_public, &sealed_len) == 0 &&
                   sealed_len == 32);

    /* The receiver answers with its own key and an encrypted proof. */
    check_true("receiver shared secret",
               hap_x25519_shared(shared_by_receiver, receiver_private, client_public) == 0);
    check_true("receiver verify key",
               hap_hkdf_sha512(shared_by_receiver, sizeof shared_by_receiver, HAP_HKDF_SALT_VERIFY,
                               HAP_HKDF_INFO_VERIFY, verify_key, sizeof verify_key) == 0);
    {
        uint8_t sub[64];
        uint8_t ciphertext[64 + 16];
        size_t ciphertext_len;
        hap_tlv_out_t build;
        hap_tlv_out_t payload;

        hap_tlv_out_init(&payload, sub, sizeof sub);
        hap_tlv_put_u8(&payload, HAP_TLV_STATE, 0x02);
        check_true("receiver seals its V2 payload",
                   hap_aead_seal_label(verify_key, "PV-Msg02", NULL, 0, sub, payload.len,
                                       ciphertext) == 0);
        ciphertext_len = payload.len + 16;

        hap_tlv_out_init(&build, v2, sizeof v2);
        hap_tlv_put_u8(&build, HAP_TLV_STATE, 0x02);
        hap_tlv_put(&build, HAP_TLV_PUBLIC_KEY, receiver_public, sizeof receiver_public);
        hap_tlv_put(&build, HAP_TLV_ENCRYPTED_DATA, ciphertext, ciphertext_len);
        len = build.len;
    }

    check_true("V2 accepted and V3 produced",
               hap_verify_v2(verify, v2, len, v3, sizeof v3, &len) == 0);

    check_true("shared secret matches the receiver side",
               hap_verify_shared_secret(verify) != NULL &&
                   memcmp(hap_verify_shared_secret(verify), shared_by_receiver, 32) == 0);
    memcpy(shared_by_client, hap_verify_shared_secret(verify), sizeof shared_by_client);

    check_true("V3 carries encrypted data",
               hap_tlv_get(v3, len, HAP_TLV_ENCRYPTED_DATA, plain, sizeof plain, &sealed_len) == 0 &&
                   sealed_len > 16);
    check_true("V3 payload opens with the verify key",
               hap_aead_open_label(verify_key, "PV-Msg03", NULL, 0, plain, sealed_len, plain) == 0);
    sealed_len -= 16;

    check_true("V3 identifies the client",
               hap_tlv_get(plain, sealed_len, HAP_TLV_IDENTIFIER, identifier, sizeof identifier,
                           &identifier_len) == 0 &&
                   identifier_len == strlen(identity.pairing_id) &&
                   memcmp(identifier, identity.pairing_id, identifier_len) == 0);

    check_true("V3 signature covers clientPub||id||serverPub",
               hap_tlv_get(plain, sealed_len, HAP_TLV_SIGNATURE, signature, sizeof signature,
                           &signature_len) == 0 &&
                   signature_len == 64 &&
                   (memcpy(sign_input + offset, client_public, 32), offset += 32,
                    memcpy(sign_input + offset, identity.pairing_id, identifier_len),
                    offset += identifier_len,
                    memcpy(sign_input + offset, receiver_public, 32), offset += 32,
                    ed25519_verify(signature, sign_input, offset, identity.public_key) == 1));

    check_true("V4 accepts an empty acknowledgement", hap_verify_v4(verify, NULL, 0) == 0);
    hap_verify_free(verify);

    /* A corrupted V2 payload must be refused. */
    verify = hap_verify_new(&identity);
    hap_verify_begin(verify, message, sizeof message, &len);
    {
        hap_tlv_out_t out;
        uint8_t buffer[128];
        uint8_t corrupted[16];

        memset(corrupted, 0, sizeof corrupted);
        hap_tlv_out_init(&out, buffer, sizeof buffer);
        hap_tlv_put_u8(&out, HAP_TLV_STATE, 0x02);
        hap_tlv_put(&out, HAP_TLV_PUBLIC_KEY, receiver_public, sizeof receiver_public);
        hap_tlv_put(&out, HAP_TLV_ENCRYPTED_DATA, corrupted, sizeof corrupted);
        check_true("corrupted V2 payload is rejected",
                   hap_verify_v2(verify, buffer, out.len, v3, sizeof v3, &len) != 0);
    }
    hap_verify_free(verify);
}

int main(void)
{
    test_tlv();
    test_mpi();
    test_srp();
    test_ed25519();
    test_x25519();
    test_aead();
    test_kdf();
    test_channel();
    test_setup();
    test_verify();

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
