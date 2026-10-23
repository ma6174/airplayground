/*
 * FairPlay SAP handshake (AirPlay "FPLY" key exchange).
 *
 * The receiver locks the stream key for a session through a two-message
 * exchange: we send m1, receive m2, derive the m3 confirmation and finally wrap
 * a random 16-byte key with the session material the receiver just proved it
 * holds. Everything the receiver can verify depends on the proprietary SAP
 * hash and white-box tables in this component, so the vectors in fp_selftest()
 * are the ground truth for the port.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FP_SAP_LEN 128
#define FP_M1_LEN 16
#define FP_M2_LEN 142
#define FP_M3_LEN 164
#define FP_M4_LEN 32
#define FP_EKEY_LEN 72
#define FP_IV_LEN 16
#define FP_RAW_KEY_LEN 16

/* Fills len bytes of entropy. Returns 0 on success. */
typedef int (*fp_random_fn)(void *ctx, uint8_t *out, size_t len);

typedef struct {
    uint8_t local_sap[FP_SAP_LEN];
    uint8_t remote_sap[FP_SAP_LEN];
    uint8_t m3[FP_M3_LEN];
    uint8_t mode;
    bool has_m3;
} fp_session_t;

/* One session spans m1, m3 and the key wrap: the SAP the receiver sends in m2
 * is the seed for every later derivative. */
int fp_session_init(fp_session_t *session, fp_random_fn random, void *ctx);

void fp_message1(const fp_session_t *session, uint8_t out[FP_M1_LEN]);

/* Consumes m2 (142 bytes) and produces m3 (164 bytes). */
int fp_exchange_m3(fp_session_t *session, const uint8_t *m2, size_t m2_len, uint8_t out[FP_M3_LEN]);

/* m4 is the receiver echoing m3[144:164]; a mismatch means the two sides
 * disagree about the SAP exchange and the stream key would be wrong. */
int fp_confirm_m4(const fp_session_t *session, const uint8_t *m4, size_t m4_len);

/* Builds the 72-byte ekey record that carries raw_key to the receiver. */
int fp_wrap_key(fp_session_t *session, const uint8_t raw_key[FP_RAW_KEY_LEN],
                fp_random_fn random, void *ctx, uint8_t out[FP_EKEY_LEN]);

/* The key the receiver will actually decrypt the stream with. A HAP pair-verify
 * session mixes its X25519 secret in; the legacy raw path keeps the bare key. */
void fp_stream_key(const uint8_t raw_key[FP_RAW_KEY_LEN], const uint8_t *pair_secret,
                   size_t pair_secret_len, bool mix_pair_secret,
                   uint8_t out[FP_RAW_KEY_LEN]);

/* Re-derives every published vector (SAP hash, white-box exchange, message
 * cipher, wrapping key, full m1->m3->ekey). Returns 0 when all match. */
int fp_selftest(void);
