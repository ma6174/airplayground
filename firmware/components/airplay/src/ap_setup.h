/*
 * SETUP negotiation: the three binary-plist request bodies and the reader for
 * the receiver's replies.
 *
 * The request shapes are protocol constants, so they are built here rather than
 * inline in the session flow; that also makes them verifiable off-device
 * against Python's plistlib.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AP_SETUP_LISTEN_PORT_NONE 0

typedef struct {
    const char *device_id;    /* "02:xx:xx:xx:xx:xx", also sent as macAddress */
    const char *session_uuid; /* upper-case UUID, reused as the RTSP Session */
    const char *name;         /* sender name shown by the receiver */
    uint16_t timing_port;     /* local UDP port the receiver's NTP probe targets */
    uint16_t audio_control_port;
    uint64_t audio_stream_id;
    uint64_t video_stream_id;
    const void *shk; /* 16-byte FairPlay stream key */
    const void *shiv;
    /* PTP timing: the session descriptor names us as a timing peer instead of
     * advertising a local NTP port. */
    bool use_ptp;
    const char *timing_peer_id;
    const char *timing_peer_address;
    /* Receivers advertising the streamConnections layout reject the legacy
     * top-level controlPort. */
    bool audio_stream_connections;
    /* 32-byte audio stream key. Modern receivers require the field even when no
     * audio packets follow. */
    const void *audio_shk;
} ap_setup_params_t;

/* All return the encoded length, or 0 when the body does not fit. */
size_t ap_setup_build_control(const ap_setup_params_t *params, void *out, size_t cap);
size_t ap_setup_build_audio(const ap_setup_params_t *params, void *out, size_t cap);
size_t ap_setup_build_video(const ap_setup_params_t *params, void *out, size_t cap);

/* Everything the sender takes from a receiver reply (SETUP or GET /info). */
typedef struct {
    bool has_event_port;
    uint16_t event_port;
    bool skip_record;
    bool has_video_data_port;
    uint16_t video_data_port;
    bool has_audio_data_port;
    uint16_t audio_data_port;
    bool has_features;
    uint64_t features;
    bool has_status_flags;
    uint64_t status_flags;
    /* PTP timeline the receiver wants frames stamped with. */
    bool has_clock_id;
    uint64_t clock_id;
    size_t display_count;
    uint32_t display_width;
    uint32_t display_height;
} ap_receiver_reply_t;

bool ap_setup_parse_reply(const void *body, size_t len, ap_receiver_reply_t *out);

/* statusFlags bit 9 asks the sender for the on-screen PIN. */
#define AP_STATUS_FLAG_PIN 0x200u
