/*
 * HAP pairing for the sender side: pair-setup (SRP-6a over TLV8) and
 * pair-verify (X25519 + Ed25519), plus the ChaCha20-Poly1305 framing that the
 * control and event connections switch to once pair-verify succeeds.
 *
 * The transport (RTSP request formatting, sockets) lives in the airplay
 * component; this component only turns one received message into the next.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HAP_ID_LEN 36
#define HAP_ED25519_SEED_LEN 32
#define HAP_ED25519_PUB_LEN 32
#define HAP_ED25519_SIG_LEN 64
#define HAP_SHARED_SECRET_LEN 32
#define HAP_KEY_LEN 32
#define HAP_TLV_MESSAGE_MAX 1024

/* Long-term controller identity. The receiver remembers this across sessions,
 * which is what lets later runs skip pair-setup entirely. */
typedef struct {
    char pairing_id[HAP_ID_LEN + 1]; /* upper-case UUID string */
    uint8_t seed[HAP_ED25519_SEED_LEN];
    uint8_t public_key[HAP_ED25519_PUB_LEN];
} hap_identity_t;

void hap_identity_generate(hap_identity_t *identity);
int hap_identity_from_seed(hap_identity_t *identity, const uint8_t seed[HAP_ED25519_SEED_LEN],
                           const char *pairing_id);
/* Signs with the identity key; used by pair-verify. */
void hap_identity_sign(const hap_identity_t *identity, const uint8_t *message, size_t message_len,
                       uint8_t signature[HAP_ED25519_SIG_LEN]);

/* pair-setup: M1 -> M2 -> M3 -> M4 -> M5 -> M6.
 * pin == NULL or "" selects transient (no-PIN) pairing. */
typedef struct hap_setup hap_setup_t;

hap_setup_t *hap_setup_new(hap_identity_t *identity, const char *pin);
void hap_setup_free(hap_setup_t *setup);
int hap_setup_begin(hap_setup_t *setup, uint8_t *out, size_t out_cap, size_t *out_len);
int hap_setup_m2(hap_setup_t *setup, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                 size_t *out_len);
int hap_setup_m4(hap_setup_t *setup, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                 size_t *out_len);
int hap_setup_m6(hap_setup_t *setup, const uint8_t *in, size_t in_len);

/* pair-verify: V1 -> V2 -> V3 -> V4. */
typedef struct hap_verify hap_verify_t;

hap_verify_t *hap_verify_new(hap_identity_t *identity);
void hap_verify_free(hap_verify_t *verify);
int hap_verify_begin(hap_verify_t *verify, uint8_t *out, size_t out_cap, size_t *out_len);
int hap_verify_v2(hap_verify_t *verify, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                  size_t *out_len);
int hap_verify_v4(hap_verify_t *verify, const uint8_t *in, size_t in_len);
/* X25519 secret; the root of the control, event and data channel keys. */
const uint8_t *hap_verify_shared_secret(const hap_verify_t *verify);

/* Channel key derivation. Labels are part of the wire protocol. */
void hap_derive_control_keys(const uint8_t shared[HAP_SHARED_SECRET_LEN], uint8_t write_key[HAP_KEY_LEN],
                             uint8_t read_key[HAP_KEY_LEN]);
void hap_derive_event_keys(const uint8_t shared[HAP_SHARED_SECRET_LEN], uint8_t write_key[HAP_KEY_LEN],
                           uint8_t read_key[HAP_KEY_LEN]);
int hap_derive_data_key(const uint8_t *ikm, size_t ikm_len, uint64_t stream_connection_id,
                        uint8_t out_key[HAP_KEY_LEN]);

/* ChaCha20-Poly1305 framing: [2-byte LE plaintext length][ciphertext][16-byte tag],
 * blocks of at most 1024 plaintext bytes, AAD is the length prefix, nonce is
 * 4 zero bytes followed by the little-endian block counter. */
typedef struct {
    int fd;
    bool encrypted;
    uint8_t write_key[HAP_KEY_LEN];
    uint8_t read_key[HAP_KEY_LEN];
    uint64_t write_nonce;
    uint64_t read_nonce;
    /* Plaintext a decrypted frame carried beyond what the caller asked for.
     * The control channel is consumed as a byte stream, so the remainder has to
     * survive until the next read instead of being dropped with the frame. */
    size_t pending_offset;
    size_t pending_len;
    uint8_t pending[HAP_TLV_MESSAGE_MAX];
} hap_channel_t;

void hap_channel_init(hap_channel_t *channel, int fd);
void hap_channel_enable_encryption(hap_channel_t *channel, const uint8_t write_key[HAP_KEY_LEN],
                                   const uint8_t read_key[HAP_KEY_LEN]);
/* Return 0 on success, -1 on socket error. */
int hap_channel_write(hap_channel_t *channel, const void *data, size_t len);
int hap_channel_read(hap_channel_t *channel, void *data, size_t len);

/* Standalone AEAD helpers (nonce = 4 zero bytes || LE64(counter)). */
int hap_aead_seal(const uint8_t key[HAP_KEY_LEN], uint64_t nonce, const uint8_t *aad, size_t aad_len,
                  const uint8_t *plaintext, size_t plaintext_len, uint8_t *ciphertext);
int hap_aead_open(const uint8_t key[HAP_KEY_LEN], uint64_t nonce, const uint8_t *aad, size_t aad_len,
                  const uint8_t *ciphertext, size_t ciphertext_len, uint8_t *plaintext);
