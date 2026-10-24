/*
 * Session state shared by the control flow, the video data channel, the event
 * channel and the NTP responder.
 *
 * One session runs at a time, so the caller of airplay_mirror() gets a single
 * static instance: it keeps the ~12 KB of protocol scratch (reply buffer,
 * request buffer, FairPlay state) out of the task stack, where it would not
 * fit.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airplay.h"
#include "ap_client.h"

/* Audio is filler that keeps the channel alive: one packet every 20 ms, which
 * is 882 samples at the 44.1 kHz rate the audio stream is declared with. The
 * RTP timestamp has to advance by that many samples, and start on the same
 * clock the video timestamps use: a receiver that schedules video against the
 * audio playhead holds the picture back forever when the two clocks disagree
 * by the age of the device. */
#include "ap_data.h"
#include "ap_event.h"
#include "ap_legacy.h"
#include "ap_ntp.h"
#include "ap_platform.h"
#include "fairplay.h"
#include "hap.h"

typedef struct ap_session {
    const airplay_config_t *config;

    ap_client_t client;

    hap_identity_t ephemeral_identity;
    hap_identity_t *identity;
    const char *hkp; /* "4" transient, "5" PIN with ACL, "3" PIN without */
    /* Feature 27: the receiver wants the legacy pairing type, which carries no
     * screen-capture ACL claim. */
    bool prefers_legacy_pairing;
    char pin[5];
    bool pin_required;

    fp_session_t fp;

    uint8_t shared[HAP_SHARED_SECRET_LEN];
    uint8_t shk[FP_RAW_KEY_LEN];
    uint8_t audio_key[32];
    uint8_t shiv[FP_IV_LEN];
    uint8_t ekey[FP_EKEY_LEN];

    char device_id[18];
    char session_uuid[37];
    char sender_name[64];

    uint64_t audio_stream_id;
    uint64_t video_stream_id;
    char uri[96];

    uint16_t event_port;
    bool skip_record;

    /* Capabilities from GET /info that steer later negotiation. */
    uint64_t features;
    bool has_features;

    /* Timing: PTP follows the receiver's timeline and needs no local responder;
     * NTP stamps our own boot clock and answers the receiver's probes. */
    bool use_ptp;
    bool force_ntp;
    uint32_t backoff_ms;
    const char *timing_protocol;
    char timing_peer_id[37];
    ap_clock_t clock;

    /* Port the control connection actually uses. Receivers that advertise an
     * AirTunes HTTP stub keep RTSP one port up, so this can differ from
     * config->port. */
    uint16_t control_port;
    int timing_fd;
    uint16_t timing_port;
    int audio_control_fd;
    uint16_t audio_control_port;

    ap_data_channel_t data;
    ap_event_channel_t event;

    /* Legacy (AirTunes) stream state: the ekey-wrapped key is unwrapped on the
     * receiver with m3, then mixed with the pair-verify secret. */
    uint8_t aeskey[FP_RAW_KEY_LEN];
    uint8_t aesiv[FP_IV_LEN];
    uint8_t legacy_skey[16];
    uint8_t legacy_siv[16];
    ap_legacy_ctr_t legacy_ctr;
    /* 0: receiver handed out a dataPort; 1-4: see legacy_data_mode. */
    uint8_t legacy_transport;
    /* RECORD already went out in the legacy front half. */
    bool legacy_recorded;
    /* Receivers that answer the mirror SETUP on a second RTSP port keep that
     * connection in this client. */
    ap_client_t stream_client;
    bool stream_client_open;

    /* Protocol scratch. Only the session task touches these buffers. */
    uint8_t msg[1024];
    uint8_t plist[1024];
    uint8_t *frame_scratch;
    size_t frame_scratch_len;

    TaskHandle_t ntp_task;
    TaskHandle_t event_task;

    volatile bool stop;
} ap_session_t;
