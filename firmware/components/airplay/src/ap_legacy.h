/*
 * Legacy AirTunes mirroring (pre-HAP): the protocol variant that third-party
 * receivers claiming AppleTV3,2 speak - raw Ed25519/X25519 pair exchanges, an
 * ekey-wrapped stream key, ANNOUNCE + two SETUPs, and AES-128-CTR sealed
 * mirror records with a keystream that runs continuously across records.
 *
 * Byte-level reference: docs/legacy-spec.md (reverse-engineered from the
 * RPiPlay receiver sources, which is what this class of receivers runs).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ap_client.h"
#include "ap_frame.h"
#include "hap.h"

struct ap_session;

/* Continuous AES-128-CTR keystream: one context per data connection, never
 * reset between records or frames. Block n of the keystream is
 * AES-ECB(key, counter + n) with the counter a big-endian 128-bit value. */
typedef struct {
    uint8_t key[16];
    uint8_t counter[16];
    uint8_t keystream[16];
    size_t used;
} ap_legacy_ctr_t;

void ap_legacy_ctr_start(ap_legacy_ctr_t *ctr, const uint8_t key[16], const uint8_t iv[16]);
/* XORs len bytes in place at the current keystream position. */
void ap_legacy_ctr_xor(ap_legacy_ctr_t *ctr, uint8_t *data, size_t len);

/* skey/siv from the stream key mixed with the pair-verify secret and the
 * decimal string of the stream connection id. */
void ap_legacy_derive_stream_keys(const uint8_t stream_key[16], const uint8_t ecdh[32],
                                  uint64_t connection_id, uint8_t skey[16], uint8_t siv[16]);

/* AES-128 single block, provided per platform (PSA on device, CommonCrypto on
 * the test host). Exposed for the derivation test vectors. */
void ap_legacy_aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/* --- session orchestration (legacy variant of the HAP front half) --- */

int ap_legacy_pair_setup(ap_client_t *client, const hap_identity_t *identity,
                         uint8_t receiver_ed[32]);
/* Runs both pair-verify exchanges; returns 0 and the X25519 secret that the
 * stream key is mixed with. receiver_ed comes from ap_legacy_pair_setup. */
int ap_legacy_pair_verify(ap_client_t *client, const hap_identity_t *identity,
                          const uint8_t receiver_ed[32], uint8_t ecdh_out[32]);

/* Builds the ANNOUNCE SDP body. */
size_t ap_legacy_build_announce(char *out, size_t cap, const char *session_uuid,
                                const char *sender_ip, const char *receiver_ip);
/* SETUP 1 body: eiv + ekey + our NTP port. */
size_t ap_legacy_build_setup1(uint8_t *out, size_t cap, const uint8_t eiv[16],
                              const uint8_t ekey[72], uint16_t timing_port);
/* SETUP 2 body: streams[{type:110, streamConnectionID}]. */
size_t ap_legacy_build_setup2(uint8_t *out, size_t cap, uint64_t connection_id);
