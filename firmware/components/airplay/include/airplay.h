/*
 * AirPlay screen-mirroring sender: the control channel, FairPlay key exchange,
 * SETUP negotiation and the video data channel.
 *
 * The session consumes one of two content sources: pre-encoded still images
 * (see the media component) or a live H.264 source that encodes at runtime on
 * the chip. The protocol layer is agnostic: both hand it access units in AVCC
 * framing plus a decoder configuration record.
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

/* A live video source: encodes access units at runtime. It is stateful and
 * single-threaded - the session task calls next_frame() once per frame, and
 * successive calls continue the stream (frame counter, GOP position). The
 * pointer returned by next_frame() stays valid until the next call. */
typedef struct {
    uint16_t width;
    uint16_t height;
    /* Target milliseconds between frames. The real pace is this or the time
     * the source needs to produce a frame, whichever is longer. */
    uint32_t interval_ms;
    /* Upper bound for one access unit, so the session can size its encryption
     * scratch once instead of probing the source. */
    size_t max_au_len;
    /* The AVCDecoderConfigurationRecord (with the trailing bytes senders
     * append). Called once per session, before the first frame. */
    const uint8_t *(*codec_record)(void *user, size_t *len);
    /* Produces the next access unit in AVCC framing. *idr tells whether the
     * receiver may start decoding here. Returns ESP_OK, or ESP_FAIL on a
     * permanent encoder error, which ends the session. */
    esp_err_t (*next_frame)(void *user, const uint8_t **au, size_t *len, bool *idr);
    void *user;
} airplay_live_source_t;

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
    /* The content source: exactly one of frames / live_source must be set.
     * frame_interval_ms applies to still images only; a live source carries
     * its own pacing. */
    const airplay_frames_t *frames;
    const airplay_live_source_t *live_source;
    uint32_t frame_interval_ms;
    /* Long-term controller identity, owned by the caller and persisted there. */
    hap_identity_t *identity;
} airplay_config_t;

/* Runs one mirroring session: pair, FairPlay, SETUP, stream still images until
 * the receiver goes away or the connection fails. Blocking. */
esp_err_t airplay_mirror(const airplay_config_t *config);

/* Politely ends the session that airplay_mirror is running (or about to run).
 * Safe to call from any task and when no session is live; the call returns
 * once the control connection has been torn down. */
void airplay_request_stop(void);

/* True between airplay_mirror being requested and it returning. */
bool airplay_is_running(void);

/* After a failed session: how long the receiver asked us to stay away. Pairing
 * failures are throttled with a HomeKit backoff, and retrying early only
 * extends it. Zero when the failure carried no backoff. */
uint32_t airplay_backoff_ms(void);
