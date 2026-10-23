/*
 * The video data channel: one TCP connection to the receiver's dataPort over
 * which every packet carries a 128-byte header.
 *
 * Access units are ChaCha20-Poly1305 sealed with the whole header as AAD, so
 * the header is finalised before encryption. Configuration and heartbeat
 * packets are never encrypted.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hap.h"

#define AP_DATA_CONNECT_TIMEOUT_MS 5000

typedef struct {
    int fd;
    bool encrypted;
    uint8_t key[HAP_KEY_LEN];
    uint64_t nonce;
} ap_data_channel_t;

int ap_data_open(ap_data_channel_t *channel, const char *host, uint16_t port,
                 const uint8_t key[HAP_KEY_LEN]);
void ap_data_close(ap_data_channel_t *channel);

/* Send the decoder configuration (avcC) unencrypted. */
int ap_data_send_codec(ap_data_channel_t *channel, const void *avcc, size_t avcc_len, uint16_t width,
                       uint16_t height, uint64_t timestamp);

/* Seal and send one AVCC access unit. scratch must hold payload_len + 16 bytes.
 * timeline_id is 0 in NTP mode and the receiver's PTP ClockID otherwise. */
int ap_data_send_frame(ap_data_channel_t *channel, const void *payload, size_t payload_len, bool idr,
                       uint64_t timestamp, uint64_t timeline_id, uint8_t *scratch,
                       size_t scratch_len);

int ap_data_send_heartbeat(ap_data_channel_t *channel);
