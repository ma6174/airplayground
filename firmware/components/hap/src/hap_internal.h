/*
 * Internal interfaces shared by the HAP component sources. Nothing here is
 * part of the component's public surface; the host tests include this header
 * directly to exercise the primitives (TLV8, big integers, SRP, X25519).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hap.h"

/* ------------------------------------------------------------------ logging */

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#define HAP_LOGE(fmt, ...) ESP_LOGE("hap", fmt, ##__VA_ARGS__)
#define HAP_LOGW(fmt, ...) ESP_LOGW("hap", fmt, ##__VA_ARGS__)
#define HAP_LOGI(fmt, ...) ESP_LOGI("hap", fmt, ##__VA_ARGS__)
#define HAP_LOGD(fmt, ...) ESP_LOGD("hap", fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define HAP_LOGE(fmt, ...) fprintf(stderr, "E hap: " fmt "\n", ##__VA_ARGS__)
#define HAP_LOGW(fmt, ...) fprintf(stderr, "W hap: " fmt "\n", ##__VA_ARGS__)
#define HAP_LOGI(fmt, ...) fprintf(stderr, "I hap: " fmt "\n", ##__VA_ARGS__)
#define HAP_LOGD(fmt, ...) fprintf(stderr, "D hap: " fmt "\n", ##__VA_ARGS__)
#endif

/* ------------------------------------------------------------------- random */

/* Implemented by third_party/ed25519/randombytes.c: esp_fill_random() on the
 * device, /dev/urandom in the host tests. */
void randombytes(unsigned char *buf, size_t len);

/* --------------------------------------------------------------------- TLV8 */

#define HAP_TLV_METHOD 0x00
#define HAP_TLV_IDENTIFIER 0x01
#define HAP_TLV_SALT 0x02
#define HAP_TLV_PUBLIC_KEY 0x03
#define HAP_TLV_PROOF 0x04
#define HAP_TLV_ENCRYPTED_DATA 0x05
#define HAP_TLV_STATE 0x06
#define HAP_TLV_ERROR 0x07
#define HAP_TLV_RETRY_DELAY 0x08
#define HAP_TLV_SIGNATURE 0x0A
#define HAP_TLV_ACL 0x12
#define HAP_TLV_FLAGS 0x13

/* A value longer than 255 bytes is split into several items carrying the same
 * tag, which is how SRP public keys and proofs travel. */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} hap_tlv_out_t;

void hap_tlv_out_init(hap_tlv_out_t *out, uint8_t *buf, size_t cap);
int hap_tlv_put(hap_tlv_out_t *out, uint8_t tag, const uint8_t *value, size_t value_len);
int hap_tlv_put_u8(hap_tlv_out_t *out, uint8_t tag, uint8_t value);

/* Concatenates every segment carrying `tag` into `value`. `*value_len` is 0
 * when the tag is absent. Returns -1 on a malformed message or when the
 * concatenation does not fit in `cap`. */
int hap_tlv_get(const uint8_t *message, size_t message_len, uint8_t tag, uint8_t *value, size_t cap,
                size_t *value_len);

/* Returns the TLV error code, or -1 when the message carries none. */
int hap_tlv_error(const uint8_t *message, size_t message_len);

/* ------------------------------------------------------------ big integers */

/* 96 x 32 bits: the 3072-bit SRP group, with room for a 64-byte hash operand. */
#define HAP_MPI_LIMBS 96
#define HAP_MPI_BYTES (HAP_MPI_LIMBS * 4)

typedef struct {
    uint32_t d[HAP_MPI_LIMBS]; /* little-endian limbs */
} hap_mpi_t;

/* Montgomery context for one odd modulus. */
typedef struct {
    hap_mpi_t n;
    hap_mpi_t r2; /* R^2 mod n, R = 2^(32*HAP_MPI_LIMBS) */
    uint32_t n0inv; /* -n^-1 mod 2^32 */
} hap_mpi_ctx_t;

int hap_mpi_ctx_init(hap_mpi_ctx_t *ctx, const uint8_t *n_be, size_t n_len);
int hap_mpi_from_bytes(hap_mpi_t *r, const uint8_t *be, size_t len);
/* Minimal big-endian export (leading zeros dropped), like Go's big.Int.Bytes().
 * Returns the byte count, or (size_t)-1 when the value does not fit in `cap`. */
size_t hap_mpi_to_bytes(const hap_mpi_t *a, uint8_t *be, size_t cap);
/* Big-endian export left-padded with zeros to `len` bytes. */
size_t hap_mpi_to_bytes_padded(const hap_mpi_t *a, uint8_t *be, size_t len);
int hap_mpi_cmp(const hap_mpi_t *a, const hap_mpi_t *b);
bool hap_mpi_is_zero(const hap_mpi_t *a);
/* r = a - b, returning 1 when the subtraction borrowed (a < b). */
int hap_mpi_sub(hap_mpi_t *r, const hap_mpi_t *a, const hap_mpi_t *b);
/* r = a + b, wrapping modulo 2^(32*HAP_MPI_LIMBS). The wrap is what makes the
 * borrowing subtraction above usable as a residue: a wrapped difference plus N
 * lands on the right value even though the addition itself overflows. */
void hap_mpi_add(hap_mpi_t *r, const hap_mpi_t *a, const hap_mpi_t *b);
/* r = a * b mod n, a < n, b < n. */
void hap_mpi_mulmod(const hap_mpi_ctx_t *ctx, hap_mpi_t *r, const hap_mpi_t *a, const hap_mpi_t *b);
/* r = base^exp mod n over a big-endian exponent. */
int hap_mpi_modexp(const hap_mpi_ctx_t *ctx, hap_mpi_t *r, const hap_mpi_t *base,
                   const uint8_t *exp_be, size_t exp_len);

/* --------------------------------------------------------------- SRP-6a */

#define HAP_SRP_WIDTH 384 /* 3072-bit group: wire width of A, B, k*g^x */
#define HAP_SRP_HASH_LEN 64

typedef struct {
    hap_mpi_ctx_t ctx;
    uint8_t padded_a[HAP_SRP_WIDTH];
    size_t a_len; /* minimal byte length of A */
    uint8_t k[HAP_SRP_HASH_LEN];
    uint8_t m1[HAP_SRP_HASH_LEN];
    uint8_t m2[HAP_SRP_HASH_LEN];
} hap_srp_t;

/* Runs the client half of pair-setup SRP up to M3.
 * `pin` is "" for transient pairing; `client_secret_a` is 32 bytes of entropy,
 * or NULL to draw them from the platform RNG. */
int hap_srp_client_start(hap_srp_t *srp, const char *pin, const uint8_t *salt, size_t salt_len,
                         const uint8_t *server_public, size_t server_public_len,
                         const uint8_t *client_secret_a);
/* Checks M4's proof against H(A, M1, K). */
int hap_srp_verify_server_proof(const hap_srp_t *srp, const uint8_t *proof, size_t proof_len);

/* ------------------------------------------------------------------- KDF */

int hap_psa_ensure_init(void);
int hap_hkdf_sha512(const uint8_t *ikm, size_t ikm_len, const char *salt, const char *info,
                    uint8_t *out, size_t out_len);

/* --------------------------------------------------------------- AEAD */

/* Pairing messages use nonce = 4 zero bytes || 8-character ASCII label
 * ("PS-Msg05", "PV-Msg02", "PV-Msg03") rather than a frame counter. */
int hap_aead_seal_label(const uint8_t key[HAP_KEY_LEN], const char *label, const uint8_t *aad,
                        size_t aad_len, const uint8_t *plaintext, size_t plaintext_len,
                        uint8_t *ciphertext);
int hap_aead_open_label(const uint8_t key[HAP_KEY_LEN], const char *label, const uint8_t *aad,
                        size_t aad_len, const uint8_t *ciphertext, size_t ciphertext_len,
                        uint8_t *plaintext);

/* --------------------------------------------------------------- X25519 */

void hap_x25519_public_key(uint8_t public_key[32], const uint8_t private_key[32]);
/* Returns -1 when the peer key is degenerate (all-zero shared secret). */
int hap_x25519_shared(uint8_t shared[32], const uint8_t private_key[32], const uint8_t peer[32]);

/* ------------------------------------------------------- variable literals */

#define HAP_SRP_USERNAME "Pair-Setup"
#define HAP_ACL_SCREEN_CAPTURE "\xe1\x57" "com.apple.ScreenCapture" "\x01"
#define HAP_ACL_SCREEN_CAPTURE_LEN 25

#define HAP_HKDF_SALT_SETUP_ENCRYPT "Pair-Setup-Encrypt-Salt"
#define HAP_HKDF_INFO_SETUP_ENCRYPT "Pair-Setup-Encrypt-Info"
#define HAP_HKDF_SALT_SETUP_SIGN "Pair-Setup-Controller-Sign-Salt"
#define HAP_HKDF_INFO_SETUP_SIGN "Pair-Setup-Controller-Sign-Info"
#define HAP_HKDF_SALT_VERIFY "Pair-Verify-Encrypt-Salt"
#define HAP_HKDF_INFO_VERIFY "Pair-Verify-Encrypt-Info"
#define HAP_HKDF_SALT_CONTROL "Control-Salt"
#define HAP_HKDF_INFO_CONTROL_WRITE "Control-Write-Encryption-Key"
#define HAP_HKDF_INFO_CONTROL_READ "Control-Read-Encryption-Key"
#define HAP_HKDF_SALT_EVENT "Events-Salt"
#define HAP_HKDF_INFO_EVENT_WRITE "Events-Write-Encryption-Key"
#define HAP_HKDF_INFO_EVENT_READ "Events-Read-Encryption-Key"
#define HAP_HKDF_SALT_DATASTREAM "DataStream-Salt"
#define HAP_HKDF_INFO_DATASTREAM "DataStream-Output-Encryption-Key"
