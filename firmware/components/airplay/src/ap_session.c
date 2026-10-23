#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airplay.h"
#include "ap_frame.h"
#include "ap_internal.h"
#include "ap_setup.h"
#include "esp_err.h"
#include "fairplay.h"
#include "hap.h"

static const char *TAG = "airplay";

#define AP_CONTENT_TYPE_PLIST "application/x-apple-binary-plist"
#define AP_CONTENT_TYPE_OCTET "application/octet-stream"

#define AP_HEARTBEAT_INTERVAL_MS 1000
#define AP_FEEDBACK_INTERVAL_MS 2000
#define AP_FEEDBACK_FAILURE_LIMIT 3
#define AP_LOOP_TICK_MS 20

/* One mirroring session at a time; the buffers live in static storage because
 * they do not fit on an ESP-IDF task stack. */
static ap_session_t s_session;

/* Set once a receiver has rejected transient pairing: the next session goes
 * straight to the on-screen-code flow instead of spending an attempt that the
 * receiver counts as a failure. */
static bool s_receiver_wants_pin;

static uint64_t random_u64(void)
{
    uint64_t value = 0;
    ap_random_bytes((uint8_t *)&value, sizeof(value));
    return value;
}

static uint64_t make_stream_id(void)
{
    return random_u64() & 0x7fffffffffffffffULL;
}

static void format_uuid(const uint8_t bytes[16], char out[37])
{
    snprintf(out, 37,
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", bytes[0],
             bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
             bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

/* Locally administered, unicast, and stable for the session. */
static void format_device_id(const uint8_t bytes[16], char out[18])
{
    snprintf(out, 18, "02:%02X:%02X:%02X:%02X:%02X", bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
}

static void sanitize_name(const char *source, char out[64])
{
    const char *text = (source != NULL && source[0] != '\0') ? source : "ESP32-S3 AirPlay";
    size_t len = 0;
    while (*text != '\0' && len + 1 < 64) {
        unsigned char c = (unsigned char)*text++;
        out[len++] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?';
    }
    while (len > 0 && out[len - 1] == ' ') {
        len--;
    }
    out[len] = '\0';
    if (len == 0) {
        snprintf(out, 64, "%s", "ESP32-S3 AirPlay");
    }
}

static esp_err_t discover(ap_session_t *session)
{
    ap_response_t response;
    if (ap_client_request(&session->client, "GET", "/info", NULL, NULL, 0, NULL, &response) != 0) {
        AP_LOGE("GET /info failed");
        return ESP_FAIL;
    }

    size_t body_len = 0;
    const void *body = ap_client_body(&session->client, &body_len);
    ap_receiver_reply_t info;
    if (!ap_setup_parse_reply(body, body_len, &info)) {
        AP_LOGE("GET /info reply is not a binary plist");
        ap_log_hex("info body", body, body_len);
        return ESP_FAIL;
    }

    if (info.has_features) {
        session->features = info.features;
        session->has_features = true;
        AP_LOGI("receiver features 0x%016llx", (unsigned long long)info.features);
    }
    if (info.has_status_flags) {
        AP_LOGI("receiver statusFlags 0x%08llx%s", (unsigned long long)info.status_flags,
                (info.status_flags & AP_STATUS_FLAG_PIN) ? " (PIN required)" : "");
        session->pin_required = (info.status_flags & AP_STATUS_FLAG_PIN) != 0;
    }
    if (info.display_count > 0) {
        AP_LOGI("receiver display 0: %ux%u (%u display(s))", (unsigned)info.display_width,
                (unsigned)info.display_height, (unsigned)info.display_count);
    }

    /* An encrypted session names itself as a PTP peer; receivers that do not
     * advertise the capability (feature bit 41) keep the NTP path. */
    session->prefers_legacy_pairing =
        session->has_features && (session->features & (1ULL << 27)) != 0;
    if (session->prefers_legacy_pairing) {
        AP_LOGI("receiver prefers the legacy pairing type (no screen-capture ACL)");
    }
    session->use_ptp = !session->force_ntp && info.has_features && (info.features & (1ULL << 41)) != 0;
    session->timing_protocol = session->use_ptp ? "PTP" : "NTP";
    AP_LOGI("timing protocol: %s", session->timing_protocol);
    return ESP_OK;
}

/* HomeKit throttles failing pairings. The reply is TLV8: 0x07 carries the error
 * code and 0x08 the wait in little-endian seconds. Receivers pair the delay
 * with an authentication failure as well as with the dedicated backoff code, and
 * retrying early only extends it, so any delay counts. */
static uint32_t pairing_backoff_ms(const uint8_t *tlv, size_t len)
{
    uint32_t seconds = 0;
    bool delayed = false;

    for (size_t index = 0; index + 1 < len;) {
        uint8_t tag = tlv[index];
        size_t length = tlv[index + 1];
        if (index + 2 + length > len) {
            break;
        }
        if (tag == 0x08) {
            delayed = true;
            seconds = 0;
            for (size_t byte = 0; byte < length && byte < 4; byte++) {
                seconds |= (uint32_t)tlv[index + 2 + byte] << (8 * byte);
            }
        }
        index += 2 + length;
    }
    return delayed ? seconds * 1000u : 0;
}

static void note_backoff(ap_session_t *session, const uint8_t *body, size_t len)
{
    uint32_t delay = pairing_backoff_ms(body, len);
    if (delay > session->backoff_ms) {
        session->backoff_ms = delay;
        AP_LOGW("receiver asked for a %u s pairing backoff", (unsigned)(delay / 1000u));
    }
}

static esp_err_t pair_setup(ap_session_t *session, const char *pin)
{
    bool include_acl = session->hkp[0] == '5';
    hap_setup_t *setup = hap_setup_new(session->identity, pin, include_acl);
    if (setup == NULL) {
        AP_LOGE("pair-setup: no session state");
        return ESP_FAIL;
    }

    ap_header_t headers[4] = {
        { "X-Apple-HKP", session->hkp },
        { "X-Apple-Client-Name", session->sender_name },
        { "X-Apple-Client-ID", session->identity->pairing_id },
        { NULL, NULL },
    };
    ap_response_t response;
    size_t message_len = 0;
    esp_err_t result = ESP_FAIL;

    if (hap_setup_begin(setup, session->msg, sizeof(session->msg), &message_len) != 0) {
        AP_LOGE("pair-setup: M1 could not be built");
        goto done;
    }

    for (int step = 0; step < 3; step++) {
        if (ap_client_request(&session->client, "POST", "/pair-setup", AP_CONTENT_TYPE_OCTET,
                              session->msg, message_len, headers, &response) != 0) {
            size_t rejected_len = 0;
            const uint8_t *rejected = ap_client_body(&session->client, &rejected_len);
            AP_LOGE("pair-setup: step %d rejected (status %d, %u body bytes)", step + 1,
                    response.status, (unsigned)rejected_len);
            ap_log_hex("pair-setup rejection", rejected, rejected_len);
            goto done;
        }

        size_t body_len = 0;
        const uint8_t *body = ap_client_body(&session->client, &body_len);
        int rc;
        if (step == 0) {
            rc = hap_setup_m2(setup, body, body_len, session->msg, sizeof(session->msg), &message_len);
        } else if (step == 1) {
            rc = hap_setup_m4(setup, body, body_len, session->msg, sizeof(session->msg), &message_len);
        } else {
            rc = hap_setup_m6(setup, body, body_len);
        }
        if (rc != 0) {
            AP_LOGE("pair-setup: M%d rejected", 2 * step + 2);
            ap_log_hex("pair-setup reply", body, body_len);
            note_backoff(session, body, body_len);
            if (session->backoff_ms != 0) {
                result = ESP_ERR_NOT_FINISHED;
            }
            goto done;
        }
        AP_LOGI("pair-setup: M%d ok", 2 * step + 2);
    }

    AP_LOGI("pair-setup complete (%s)", (pin != NULL && pin[0] != '\0') ? "PIN" : "transient");
    result = ESP_OK;

done:
    hap_setup_free(setup);
    return result;
}

static esp_err_t pair_verify(ap_session_t *session)
{
    hap_verify_t *verify = hap_verify_new(session->identity);
    if (verify == NULL) {
        AP_LOGE("pair-verify: no session state");
        return ESP_FAIL;
    }

    ap_header_t headers[5] = {
        { "X-Apple-HKP", session->hkp },
        { "X-Apple-Client-Name", session->sender_name },
        { "X-Apple-Client-ID", session->identity->pairing_id },
        { "X-Apple-PD", "1" },
        { NULL, NULL },
    };
    ap_response_t response;
    size_t message_len = 0;
    esp_err_t result = ESP_FAIL;

    if (hap_verify_begin(verify, session->msg, sizeof(session->msg), &message_len) != 0) {
        AP_LOGE("pair-verify: V1 could not be built");
        goto done;
    }

    for (int step = 0; step < 2; step++) {
        if (ap_client_request(&session->client, "POST", "/pair-verify", AP_CONTENT_TYPE_OCTET,
                              session->msg, message_len, headers, &response) != 0) {
            AP_LOGE("pair-verify: step %d rejected (status %d)", step + 1, response.status);
            goto done;
        }

        size_t body_len = 0;
        const uint8_t *body = ap_client_body(&session->client, &body_len);
        int rc;
        if (step == 0) {
            rc = hap_verify_v2(verify, body, body_len, session->msg, sizeof(session->msg), &message_len);
        } else {
            rc = hap_verify_v4(verify, body, body_len);
        }
        if (rc != 0) {
            AP_LOGE("pair-verify: V%d rejected", 2 * step + 2);
            ap_log_hex("pair-verify reply", body, body_len);
            goto done;
        }
    }

    const uint8_t *shared = hap_verify_shared_secret(verify);
    if (shared == NULL) {
        AP_LOGE("pair-verify: no shared secret");
        goto done;
    }
    memcpy(session->shared, shared, sizeof(session->shared));

    uint8_t write_key[HAP_KEY_LEN];
    uint8_t read_key[HAP_KEY_LEN];
    hap_derive_control_keys(session->shared, write_key, read_key);
    ap_client_enable_encryption(&session->client, write_key, read_key);

    AP_LOGI("pair-verify complete");
    result = ESP_OK;

done:
    hap_verify_free(verify);
    return result;
}

static int fairplay_random(void *context, uint8_t *out, size_t len)
{
    (void)context;
    ap_random_bytes(out, len);
    return 0;
}

/* Asks the application for the code the receiver is showing. */
static const char *ask_for_pin(ap_session_t *session)
{
    char entered[5] = { 0 };

    if (session->config->request_pin == NULL ||
        !session->config->request_pin(entered, session->config->user_data)) {
        return NULL;
    }
    entered[4] = '\0';
    memcpy(session->pin, entered, sizeof(session->pin));
    ap_client_set_password(&session->client, session->pin);
    AP_LOGI("pairing code collected from the user");
    return session->pin;
}

/* Triggers the on-screen code. 453 means the receiver accepted the request but
 * will display it later, which is not an error. */
static esp_err_t show_pin_on_receiver(ap_session_t *session)
{
    ap_header_t headers[5] = {
        { "X-Apple-HKP", session->hkp },
        { "X-Apple-Client-Name", session->sender_name },
        { "X-Apple-Client-ID", session->identity->pairing_id },
        { "X-Apple-SupportedPINLengths", "4" },
        { NULL, NULL },
    };
    ap_response_t response;
    int rc = ap_client_request(&session->client, "POST", "/pair-pin-start", NULL, NULL, 0, headers,
                              &response);
    if (rc == 0) {
        return ESP_OK;
    }
    if (response.status == 453) {
        AP_LOGI("pair-pin-start accepted asynchronously (453)");
        return ESP_OK;
    }
    AP_LOGE("pair-pin-start failed (status %d)", response.status);
    return ESP_FAIL;
}

/* The receiver only displays a code once /pair-pin-start arrives, so that call
 * has to come before asking the user for the digits. */
static const char *request_code(ap_session_t *session)
{
    if (!session->prefers_legacy_pairing) {
        session->hkp = "5";
    }
    if (show_pin_on_receiver(session) != ESP_OK) {
        return NULL;
    }
    return ask_for_pin(session);
}

static esp_err_t pair_and_verify(ap_session_t *session)
{
    const char *pin = NULL;

    bool want_pin = session->pin_required || session->config->force_pin || s_receiver_wants_pin;

    if (session->config->pin != NULL && session->config->pin[0] != '\0') {
        pin = session->config->pin;
        session->hkp = session->prefers_legacy_pairing ? "3" : "5";
    } else if (want_pin) {
        session->hkp = session->prefers_legacy_pairing ? "3" : "5";
        pin = request_code(session);
        if (pin == NULL) {
            AP_LOGE("receiver requires a pairing code and none was supplied");
            return ESP_FAIL;
        }
    } else {
        session->hkp = "4";
    }

    /* A persisted identity may already be paired, in which case the receiver
     * accepts pair-verify straight away and the user sees no PIN prompt. */
    if (session->config->identity != NULL && pin == NULL) {
        if (pair_verify(session) == ESP_OK) {
            AP_LOGI("known identity verified without pair-setup");
            return ESP_OK;
        }
        AP_LOGW("pair-verify alone was rejected; running pair-setup");
    }

    esp_err_t setup_result = pair_setup(session, pin);
    if (setup_result == ESP_ERR_NOT_FINISHED) {
        return setup_result;
    }
    if (setup_result != ESP_OK) {
        if (pin != NULL) {
            return ESP_FAIL;
        }
        /* Transient pairing is the fast path; when the receiver rejects it, it
         * wants a code instead. Remember that for the next session, because the
         * rejected attempt has just started a pairing backoff. */
        AP_LOGW("transient pair-setup was rejected; the receiver wants a pairing code");
        s_receiver_wants_pin = true;
        pin = request_code(session);
        if (pin == NULL) {
            return ESP_FAIL;
        }
        setup_result = pair_setup(session, pin);
        if (setup_result != ESP_OK) {
            return setup_result;
        }
    }
    return pair_verify(session);
}

static esp_err_t fairplay_setup(ap_session_t *session)
{
    if (fp_session_init(&session->fp, fairplay_random, NULL) != 0) {
        AP_LOGE("FairPlay: session init failed");
        return ESP_FAIL;
    }

    ap_header_t headers[2] = {
        { "X-Apple-ET", "32" },
        { NULL, NULL },
    };
    ap_response_t response;

    uint8_t m1[FP_M1_LEN];
    fp_message1(&session->fp, m1);
    if (ap_client_request(&session->client, "POST", "/fp-setup", AP_CONTENT_TYPE_OCTET, m1, sizeof(m1),
                          headers, &response) != 0) {
        AP_LOGE("FairPlay: m1 rejected");
        return ESP_FAIL;
    }
    size_t body_len = 0;
    const uint8_t *m2 = ap_client_body(&session->client, &body_len);
    if (body_len != FP_M2_LEN) {
        AP_LOGE("FairPlay: m2 is %u bytes, expected %u", (unsigned)body_len, (unsigned)FP_M2_LEN);
        return ESP_FAIL;
    }

    uint8_t m3[FP_M3_LEN];
    if (fp_exchange_m3(&session->fp, m2, body_len, m3) != 0) {
        AP_LOGE("FairPlay: m3 derivation failed");
        return ESP_FAIL;
    }
    if (ap_client_request(&session->client, "POST", "/fp-setup", AP_CONTENT_TYPE_OCTET, m3, sizeof(m3),
                          headers, &response) != 0) {
        AP_LOGE("FairPlay: m3 rejected");
        return ESP_FAIL;
    }
    const uint8_t *m4 = ap_client_body(&session->client, &body_len);
    if (body_len != FP_M4_LEN || fp_confirm_m4(&session->fp, m4, body_len) != 0) {
        AP_LOGE("FairPlay: m4 confirmation failed (%u bytes)", (unsigned)body_len);
        return ESP_FAIL;
    }

    uint8_t raw_key[FP_RAW_KEY_LEN];
    ap_random_bytes(raw_key, sizeof(raw_key));
    ap_random_bytes(session->shiv, sizeof(session->shiv));
    /* The HAP pair-verify secret is mixed into the FairPlay key; that combined
     * value is what the receiver installs as shk. The wrapped ekey record is a
     * legacy-receiver field and is not sent on an encrypted session. */
    fp_stream_key(raw_key, session->shared, sizeof(session->shared), true, session->shk);

    AP_LOGI("FairPlay SAP handshake complete");
    return ESP_OK;
}

static esp_err_t start_timing(ap_session_t *session)
{
    if (session->use_ptp) {
        /* PTP needs no local responder: frames are stamped on the receiver's
         * timeline, anchored when its SETUP reply arrives. */
        return ESP_OK;
    }
    session->timing_fd = ap_ntp_open(&session->timing_port);
    if (session->timing_fd < 0) {
        AP_LOGE("could not bind the NTP timing port");
        return ESP_FAIL;
    }
    session->audio_control_fd = ap_ntp_open(&session->audio_control_port);
    if (session->audio_control_fd < 0) {
        AP_LOGE("could not bind the audio control port");
        return ESP_FAIL;
    }
    AP_LOGI("local UDP ports: timing=%u audioControl=%u", (unsigned)session->timing_port,
            (unsigned)session->audio_control_port);

    if (ap_ntp_start(session) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t request_setup(ap_session_t *session, const char *uri, const void *body, size_t body_len,
                               const char *phase)
{
    ap_response_t response;
    if (ap_client_request(&session->client, "SETUP", uri, AP_CONTENT_TYPE_PLIST, body, body_len, NULL,
                          &response) != 0) {
        AP_LOGE("%s SETUP failed (status %d)", phase, response.status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool parse_reply(ap_session_t *session, ap_receiver_reply_t *reply, const char *phase)
{
    size_t body_len = 0;
    const void *body = ap_client_body(&session->client, &body_len);
    if (!ap_setup_parse_reply(body, body_len, reply)) {
        AP_LOGE("%s SETUP reply is not a binary plist", phase);
        ap_log_hex("SETUP reply", body, body_len);
        return false;
    }
    if (reply->has_event_port && reply->event_port > 0) {
        session->event_port = reply->event_port;
    }
    return true;
}

static esp_err_t setup_control(ap_session_t *session)
{
    ap_setup_params_t params = {
        .device_id = session->device_id,
        .session_uuid = session->session_uuid,
        .name = session->sender_name,
        .timing_port = session->timing_port,
        .audio_control_port = session->audio_control_port,
        .audio_stream_id = session->audio_stream_id,
        .video_stream_id = session->video_stream_id,
        .shk = session->shk,
        .shiv = session->shiv,
        .use_ptp = session->use_ptp,
        .timing_peer_id = session->timing_peer_id,
        .timing_peer_address =
            session->config->local_address != NULL ? session->config->local_address : "0.0.0.0",
    };
    size_t plist_len = ap_setup_build_control(&params, session->plist, sizeof(session->plist));
    if (plist_len == 0) {
        AP_LOGE("control SETUP body does not fit %u bytes", (unsigned)sizeof(session->plist));
        return ESP_FAIL;
    }

    if (request_setup(session, session->uri, session->plist, plist_len, "control") != ESP_OK) {
        /* A PTP session descriptor is the only part of the negotiation a
         * receiver can reject on protocol version alone, so it is the one case
         * worth retrying with the older timing protocol. */
        return session->use_ptp ? ESP_ERR_NOT_SUPPORTED : ESP_FAIL;
    }
    ap_receiver_reply_t reply;
    if (!parse_reply(session, &reply, "control")) {
        return ESP_FAIL;
    }
    session->skip_record = reply.skip_record;
    AP_LOGI("control SETUP: eventPort=%u skipRecord=%d", (unsigned)session->event_port,
            session->skip_record ? 1 : 0);
    if (reply.has_features) {
        AP_LOGI("receiver session features 0x%016llx", (unsigned long long)reply.features);
    }

    if (session->use_ptp) {
        if (!reply.has_clock_id) {
            AP_LOGE("PTP session reply carries no timingPeerInfo.ClockID");
            return ESP_ERR_NOT_SUPPORTED;
        }
        bool have_receiver_time = session->client.has_clock_headers;
        ap_clock_anchor_ptp(&session->clock, reply.clock_id, have_receiver_time,
                            session->client.received_timestamp_ms,
                            session->client.processing_time_ms);
        AP_LOGI("PTP clock: timeline=0x%016llx anchored on the %s", (unsigned long long)reply.clock_id,
                have_receiver_time ? "receiver clock" : "local boot clock");
    } else {
        ap_clock_use_ntp(&session->clock);
        AP_LOGI("NTP clock: local boot clock, timeline 0");
    }
    return ESP_OK;
}

static esp_err_t setup_audio(ap_session_t *session)
{
    ap_setup_params_t params = {
        .device_id = session->device_id,
        .session_uuid = session->session_uuid,
        .name = session->sender_name,
        .timing_port = session->timing_port,
        .audio_control_port = session->audio_control_port,
        .audio_stream_id = session->audio_stream_id,
        .video_stream_id = session->video_stream_id,
        .shk = session->shk,
        .shiv = session->shiv,
        .audio_stream_connections = session->has_features && (session->features & (1ULL << 59)) != 0,
        .audio_shk = session->audio_key,
    };
    size_t plist_len = ap_setup_build_audio(&params, session->plist, sizeof(session->plist));
    if (plist_len == 0) {
        AP_LOGE("audio SETUP body does not fit %u bytes", (unsigned)sizeof(session->plist));
        return ESP_FAIL;
    }

    if (request_setup(session, session->uri, session->plist, plist_len, "audio") != ESP_OK) {
        return ESP_FAIL;
    }
    ap_receiver_reply_t reply;
    if (!parse_reply(session, &reply, "audio")) {
        return ESP_FAIL;
    }
    AP_LOGI("audio SETUP: controlPort=%u%s", (unsigned)session->audio_control_port,
            reply.has_audio_data_port ? " (receiver assigned a data port)" : "");
    return ESP_OK;
}

static esp_err_t setup_video(ap_session_t *session, uint16_t *data_port)
{
    ap_setup_params_t params = {
        .device_id = session->device_id,
        .session_uuid = session->session_uuid,
        .name = session->sender_name,
        .timing_port = session->timing_port,
        .audio_control_port = session->audio_control_port,
        .audio_stream_id = session->audio_stream_id,
        .video_stream_id = session->video_stream_id,
        .shk = session->shk,
        .shiv = session->shiv,
    };
    size_t plist_len = ap_setup_build_video(&params, session->plist, sizeof(session->plist));
    if (plist_len == 0) {
        AP_LOGE("video SETUP body does not fit %u bytes", (unsigned)sizeof(session->plist));
        return ESP_FAIL;
    }

    char uri[96];
    int uri_len = snprintf(uri, sizeof(uri), "rtsp://%s:%u/%llu", session->config->host,
                           (unsigned)session->config->port,
                           (unsigned long long)session->video_stream_id);
    if (uri_len <= 0 || (size_t)uri_len >= sizeof(uri)) {
        AP_LOGE("receiver host name is too long for the video RTSP URI");
        return ESP_FAIL;
    }

    if (request_setup(session, uri, session->plist, plist_len, "video") != ESP_OK) {
        return ESP_FAIL;
    }
    ap_receiver_reply_t reply;
    if (!parse_reply(session, &reply, "video")) {
        return ESP_FAIL;
    }
    if (!reply.has_video_data_port || reply.video_data_port == 0) {
        AP_LOGE("video SETUP reply carries no dataPort");
        return ESP_FAIL;
    }
    *data_port = reply.video_data_port;
    AP_LOGI("video SETUP: dataPort=%u", (unsigned)*data_port);
    return ESP_OK;
}

static esp_err_t allocate_frame_scratch(ap_session_t *session)
{
    const airplay_frames_t *frames = session->config->frames;
    size_t largest = 0;
    for (size_t i = 0; i < frames->frame_count; i++) {
        size_t len = 0;
        if (frames->frame(i, &len) == NULL || len == 0) {
            AP_LOGE("still frame %u is empty", (unsigned)i);
            return ESP_FAIL;
        }
        if (len > largest) {
            largest = len;
        }
    }

    /* One allocation for the sealed access unit: ChaCha20-Poly1305 output
     * cannot alias the flash-resident plaintext. Sized once to the largest
     * frame, never per frame. */
    session->frame_scratch_len = largest + AP_FRAME_TAG_LEN;
    session->frame_scratch = (uint8_t *)malloc(session->frame_scratch_len);
    if (session->frame_scratch == NULL) {
        AP_LOGE("no memory for a %u byte frame buffer", (unsigned)session->frame_scratch_len);
        return ESP_FAIL;
    }
    AP_LOGI("frame buffer: %u bytes (largest access unit %u)", (unsigned)session->frame_scratch_len,
            (unsigned)largest);
    return ESP_OK;
}

static int send_feedback(ap_session_t *session)
{
    ap_response_t response;
    return ap_client_request(&session->client, "POST", "/feedback", NULL, NULL, 0, NULL, &response);
}

static esp_err_t stream_frames(ap_session_t *session)
{
    const airplay_frames_t *frames = session->config->frames;
    uint32_t interval = session->config->frame_interval_ms;
    if (interval == 0) {
        interval = 1000;
    }

    uint64_t timeline_id = 0;
    if (ap_data_send_codec(&session->data, frames->avcc, frames->avcc_len, frames->width, frames->height,
                           ap_clock_now(&session->clock, &timeline_id)) != 0) {
        AP_LOGE("codec packet write failed");
        return ESP_FAIL;
    }
    AP_LOGI("codec packet sent: avcC %u bytes, %ux%u", (unsigned)frames->avcc_len, (unsigned)frames->width,
            (unsigned)frames->height);

    size_t index = 0;
    uint64_t last_timestamp = 0;
    uint64_t next_frame = ap_millis();
    uint64_t next_heartbeat = 0;
    uint64_t next_feedback = ap_millis();
    bool first_frame_sent = false;
    int feedback_failures = 0;
    esp_err_t result = ESP_OK;

    while (!session->stop) {
        uint64_t now = ap_millis();

        if (now >= next_feedback) {
            if (send_feedback(session) == 0) {
                feedback_failures = 0;
            } else if (++feedback_failures >= AP_FEEDBACK_FAILURE_LIMIT) {
                AP_LOGE("feedback failed %d times in a row; ending the session", feedback_failures);
                result = ESP_FAIL;
                break;
            }
            next_feedback = now + AP_FEEDBACK_INTERVAL_MS;
        }

        if (first_frame_sent && now >= next_heartbeat) {
            if (ap_data_send_heartbeat(&session->data) != 0) {
                AP_LOGE("data channel heartbeat failed");
                result = ESP_FAIL;
                break;
            }
            next_heartbeat = now + AP_HEARTBEAT_INTERVAL_MS;
        }

        if (now >= next_frame) {
            size_t payload_len = 0;
            const uint8_t *payload = frames->frame(index, &payload_len);
            if (payload == NULL || payload_len == 0) {
                AP_LOGE("still frame %u disappeared", (unsigned)index);
                return ESP_FAIL;
            }

            uint64_t timestamp = ap_clock_now(&session->clock, &timeline_id);
            if (timestamp <= last_timestamp) {
                timestamp = last_timestamp + 1;
            }
            last_timestamp = timestamp;

            bool idr = ap_avcc_has_idr(payload, payload_len);
            if (ap_data_send_frame(&session->data, payload, payload_len, idr, timestamp, timeline_id,
                                   session->frame_scratch, session->frame_scratch_len) != 0) {
                AP_LOGE("video frame %u write failed", (unsigned)index);
                result = ESP_FAIL;
                break;
            }
            if (!first_frame_sent) {
                first_frame_sent = true;
                next_heartbeat = ap_millis() + AP_HEARTBEAT_INTERVAL_MS;
            }
            AP_LOGI("frame %u sent: %u bytes, idr=%d, timestamp=%llu", (unsigned)index,
                    (unsigned)payload_len, idr ? 1 : 0, (unsigned long long)timestamp);

            index = (index + 1) % frames->frame_count;
            next_frame = now + interval;
        }

        vTaskDelay(pdMS_TO_TICKS(AP_LOOP_TICK_MS));
    }

    return result;
}

static esp_err_t session_run(ap_session_t *session)
{
    if (ap_client_connect(&session->client, session->config->host, session->config->port,
                          AP_CONTROL_CONNECT_TIMEOUT_MS) != 0) {
        AP_LOGE("cannot connect to %s:%u", session->config->host, (unsigned)session->config->port);
        return ESP_FAIL;
    }
    /* Receivers that protect the session with "Require Password" answer with a
     * Digest challenge, and the value they expect is the same code the user
     * typed for pairing. */
    if (session->config->pin != NULL && session->config->pin[0] != '\0') {
        ap_client_set_password(&session->client, session->config->pin);
    }

    if (discover(session) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t pairing = pair_and_verify(session);
    if (pairing != ESP_OK) {
        /* A pairing backoff has to reach the caller, which then waits it out. */
        return pairing;
    }
    if (fairplay_setup(session) != ESP_OK) {
        return ESP_FAIL;
    }
    if (start_timing(session) != ESP_OK) {
        return ESP_FAIL;
    }

    if (setup_control(session) != ESP_OK) {
        return ESP_FAIL;
    }
    if (session->event_port != 0 && ap_event_start(session, session->event_port) != 0) {
        AP_LOGW("continuing without the event channel");
    }

    if (setup_audio(session) != ESP_OK) {
        return ESP_FAIL;
    }

    uint16_t data_port = 0;
    if (setup_video(session, &data_port) != ESP_OK) {
        return ESP_FAIL;
    }
    if (session->event_port != 0 && !session->event.active &&
        ap_event_start(session, session->event_port) != 0) {
        AP_LOGW("continuing without the event channel");
    }

    uint8_t data_key[HAP_KEY_LEN];
    if (hap_derive_data_key(session->shared, sizeof(session->shared), session->video_stream_id,
                            data_key) != 0) {
        AP_LOGE("video data key derivation failed");
        return ESP_FAIL;
    }
    if (ap_data_open(&session->data, session->config->host, data_port, data_key) != 0) {
        AP_LOGE("cannot connect to the video data port %u", (unsigned)data_port);
        return ESP_FAIL;
    }
    AP_LOGI("video data channel connected to port %u", (unsigned)data_port);

    if (session->skip_record) {
        AP_LOGI("receiver asked to skip RECORD; the session is already active");
    } else {
        ap_header_t headers[4] = {
            { "Session", session->session_uuid },
            { "Range", "npt=0-" },
            { "RTP-Info", "seq=0;rtptime=0" },
            { NULL, NULL },
        };
        ap_response_t response;
        if (ap_client_request(&session->client, "RECORD", session->uri, NULL, NULL, 0, headers,
                              &response) != 0) {
            AP_LOGE("RECORD failed (status %d)", response.status);
            return ESP_FAIL;
        }
    }

    if (allocate_frame_scratch(session) != ESP_OK) {
        return ESP_FAIL;
    }

    esp_err_t result = stream_frames(session);

    ap_response_t response;
    (void)ap_client_request(&session->client, "TEARDOWN", session->uri, NULL, NULL, 0, NULL, &response);
    return result;
}

static void session_teardown(ap_session_t *session)
{
    session->stop = true;

    ap_event_stop(session);
    if (session->timing_fd >= 0) {
        ap_ntp_close(session->timing_fd);
        session->timing_fd = -1;
    }
    if (session->audio_control_fd >= 0) {
        ap_socket_close(session->audio_control_fd);
        session->audio_control_fd = -1;
    }
    ap_data_close(&session->data);

    /* Give the NTP and event tasks a moment to observe the stop flag before the
     * control connection goes away. */
    vTaskDelay(pdMS_TO_TICKS(100));

    ap_client_close(&session->client);

    if (session->frame_scratch != NULL) {
        free(session->frame_scratch);
        session->frame_scratch = NULL;
        session->frame_scratch_len = 0;
    }
}

uint32_t airplay_backoff_ms(void)
{
    return s_session.backoff_ms;
}

esp_err_t airplay_mirror(const airplay_config_t *config)
{
    if (config == NULL || config->host == NULL || config->frames == NULL ||
        config->frames->frame == NULL || config->frames->frame_count == 0 ||
        config->frames->avcc == NULL || config->frames->avcc_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ap_session_t *session = &s_session;
    memset(session, 0, sizeof(*session));
    session->config = config;
    session->client.fd = -1;
    session->data.fd = -1;
    session->event.fd = -1;
    session->timing_fd = -1;
    session->audio_control_fd = -1;

    uint8_t id_bytes[16];
    ap_random_bytes(id_bytes, sizeof(id_bytes));
    format_device_id(id_bytes, session->device_id);
    ap_random_bytes(id_bytes, sizeof(id_bytes));
    id_bytes[6] = (uint8_t)((id_bytes[6] & 0x0f) | 0x40); /* version 4 */
    id_bytes[8] = (uint8_t)((id_bytes[8] & 0x3f) | 0x80); /* variant 10 */
    format_uuid(id_bytes, session->session_uuid);
    ap_random_bytes(id_bytes, sizeof(id_bytes));
    id_bytes[6] = (uint8_t)((id_bytes[6] & 0x0f) | 0x40);
    id_bytes[8] = (uint8_t)((id_bytes[8] & 0x3f) | 0x80);
    format_uuid(id_bytes, session->timing_peer_id);
    sanitize_name(config->device_name, session->sender_name);

    /* No audio packets are sent, but an encrypted session still has to publish a
     * 32-byte audio stream key. */
    ap_random_bytes(session->audio_key, sizeof(session->audio_key));
    session->audio_stream_id = make_stream_id();
    session->video_stream_id = make_stream_id();
    int uri_len = snprintf(session->uri, sizeof(session->uri), "rtsp://%s:%u/%llu", config->host,
                           (unsigned)config->port, (unsigned long long)session->audio_stream_id);
    if (uri_len <= 0 || (size_t)uri_len >= sizeof(session->uri)) {
        AP_LOGE("receiver host name is too long for the RTSP URI");
        return ESP_ERR_INVALID_ARG;
    }

    if (config->identity != NULL) {
        if (config->identity->pairing_id[0] == '\0') {
            AP_LOGE("config->identity has no pairing id");
            return ESP_ERR_INVALID_ARG;
        }
        session->identity = config->identity;
    } else {
        hap_identity_generate(&session->ephemeral_identity);
        session->identity = &session->ephemeral_identity;
    }

    AP_LOGI("mirroring to %s:%u as \"%s\" (%s)", config->host, (unsigned)config->port,
            session->sender_name, session->device_id);
    AP_LOGI("session %s, audio stream %llu, video stream %llu", session->session_uuid,
            (unsigned long long)session->audio_stream_id,
            (unsigned long long)session->video_stream_id);

    esp_err_t result = session_run(session);
    if (result == ESP_ERR_NOT_SUPPORTED && !session->force_ntp) {
        /* The receiver would not take a PTP session descriptor. Start over on a
         * fresh connection with the older timing protocol rather than leaving
         * the user with a device that cannot connect to their TV. */
        AP_LOGW("receiver rejected PTP timing; retrying the session with NTP");
        session_teardown(session);
        session->force_ntp = true;
        session->stop = false;
        result = session_run(session);
    }
    session_teardown(session);
    if (result != ESP_ERR_NOT_FINISHED) {
        session->backoff_ms = 0;
    }

    if (result != ESP_OK) {
        AP_LOGE("mirroring session ended with an error");
    } else {
        AP_LOGI("mirroring session ended");
    }
    return result;
}
