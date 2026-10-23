/*
 * AirPlay screen-mirroring sender: the control channel, FairPlay key exchange,
 * SETUP negotiation and the video data channel.
 *
 * The content source is deliberately dumb: a small set of pre-encoded H.264
 * still images. Nothing here needs an encoder, which is exactly the point on an
 * ESP32-S3 - the device proves the protocol, the host proves the codec.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "hap.h"

/* Pre-encoded still images, all from one encoder session so they share the
 * decoder configuration record (avcC) and SPS/PPS. */
typedef struct {
    const uint8_t *avcc;    /* AVCDecoderConfigurationRecord */
    size_t avcc_len;
    uint16_t width;
    uint16_t height;
    size_t frame_count;
    /* Returns one access unit, AVCC framing (4-byte big-endian NAL lengths). */
    const uint8_t *(*frame)(size_t index, size_t *len);
} airplay_frames_t;

typedef struct {
    const char *host;
    uint16_t port;
    const char *device_name;
    /* Our own address on the network the receiver can see; advertised as the
     * PTP timing peer address. */
    const char *local_address;
    /* NULL or "" selects transient pairing; otherwise the 4-digit code shown
     * on the receiver's screen. */
    const char *pin;
    /* Start with the on-screen-code flow instead of probing transient pairing.
     * Receivers that show a code reject an empty-PIN attempt *and* start a
     * pairing backoff, which then blocks the attempt that carries the code. */
    bool force_pin;
    /* Called when the receiver demands a PIN and config->pin is empty: returns
     * the code the user typed, or false to abort the session. */
    bool (*request_pin)(char pin[5], void *user_data);
    void *user_data;
    const airplay_frames_t *frames;
    uint32_t frame_interval_ms;
    /* Long-term controller identity, owned by the caller and persisted there. */
    hap_identity_t *identity;
} airplay_config_t;

/* Runs one mirroring session: pair, FairPlay, SETUP, stream still images until
 * the receiver goes away or the connection fails. Blocking. */
esp_err_t airplay_mirror(const airplay_config_t *config);

/* After a failed session: how long the receiver asked us to stay away. Pairing
 * failures are throttled with a HomeKit backoff, and retrying early only
 * extends it. Zero when the failure carried no backoff. */
uint32_t airplay_backoff_ms(void);
