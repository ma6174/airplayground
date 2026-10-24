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

/* "AirPlayStreamKey" plus a 20-digit decimal connection id. Receivers hash the
 * label in full, so this buffer must never truncate it. */
#define AP_LEGACY_LABEL_MAX 40

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
/* Mixing modes for the receiver-side pair secret:
 *   0 bare key (no secret), 1 the pair-verify secret, 2 an all-zero secret,
 *   3 raw keys (skey = stream key, siv = eiv, no hashing at all). */
void ap_legacy_derive_stream_keys(const uint8_t stream_key[16], const uint8_t ecdh[32],
                                  uint8_t mode, uint64_t connection_id, uint8_t skey[16],
                                  uint8_t siv[16]);

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

/* Builds one audio packet (RTP header + CBC payload). */
int ap_legacy_audio_packet(uint8_t *out, size_t cap, uint16_t seq, uint32_t rtp_time,
                           const uint8_t key[16], const uint8_t iv[16], size_t payload_len);

/* Writes one mirror record: [128-byte header][payload]. In interleaved mode the
 * record is wrapped in the RTSP `$`+channel+length framing, all on data_fd. */
int ap_legacy_write_record(int fd, bool interleaved, uint8_t channel,
                           const uint8_t header[AP_FRAME_HEADER_LEN], const uint8_t *payload,
                           size_t payload_len);

/* Builds the ANNOUNCE SDP body. */
size_t ap_legacy_build_announce(char *out, size_t cap, const char *session_uuid,
                                const char *sender_ip, const char *receiver_ip);
/* SETUP 1 body: eiv + ekey + our NTP port. ekey == NULL omits the field, for
 * receivers that answer fp-setup with an empty body (no FairPlay at all). */
typedef struct {
    const uint8_t *eiv;
    const uint8_t *ekey; /* NULL omits the field (no FairPlay) */
    uint16_t timing_port;
    const char *session_uuid;
    const char *session_correlation_uuid; /* iOS sends both session ids */
    const char *device_id;                /* stable per device, MAC shaped */
    const char *mac_address;              /* the interface the session runs on */
} ap_legacy_setup1_t;

size_t ap_legacy_build_setup1(uint8_t *out, size_t cap, const ap_legacy_setup1_t *setup);
/* SETUP 2 body: streams[{type:110, streamConnectionID}]. */
/* Mirror SETUP. With audio the request also asks for the type 96 stream; iOS
 * sends both, and receivers of this class wait for the audio stream before
 * they start rendering. */
size_t ap_legacy_build_setup2(uint8_t *out, size_t cap, uint64_t connection_id, bool audio,
                              uint16_t audio_control_port);
