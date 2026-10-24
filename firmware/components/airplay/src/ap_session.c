#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airplay.h"
#include "ap_frame.h"
#include "ap_internal.h"
#include "ap_legacy.h"
#include "ap_plist.h"
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
/* The loop wakes often enough to place a frame within a couple of milliseconds
 * of its deadline; a 20 ms tick quantised 30 fps into 40 ms. */
#define AP_LOOP_TICK_MS 5
/* Feedback and heartbeats ride on the control connection and block while they
 * wait for the reply, so they only go out when the next frame is far enough
 * away to absorb the round trip. */
#define AP_SLACK_FOR_CONTROL_MS 120
#define AP_FEEDBACK_MAX_DEFERRALS 10

/* One mirroring session at a time; the buffers live in static storage because
 * they do not fit on an ESP-IDF task stack. */
static ap_session_t s_session;

/* Set once a receiver has rejected transient pairing: the next session goes
 * straight to the on-screen-code flow instead of spending an attempt that the
 * receiver counts as a failure. */
static bool s_receiver_wants_pin;

/* Session state shared with whoever may call airplay_request_stop() from
 * another task. Plain volatile bools: single writer each, and the frame loop
 * only needs to see the value eventually. */
static volatile bool s_mirroring;
static volatile bool s_stop_requested;
/* Set once the session delivers a frame: proof the receiver is a working one
 * and not an HTTP stub that answers everything with an empty body. */
static volatile bool s_streamed;

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

    /* Every pairing request carries this header, including the pair-verify
     * attempt below, so it needs a value before the first request goes out. */
    session->hkp = session->prefers_legacy_pairing ? "3" : "4";

    /* A receiver that has paired with us before accepts pair-verify straight
     * away, and a rejected pair-verify does not start a pairing backoff, so this
     * is always the first thing to try - even for receivers that want a code. */
    if (session->config->identity != NULL) {
        if (pair_verify(session) == ESP_OK) {
            AP_LOGI("known identity verified without pair-setup");
            return ESP_OK;
        }
        AP_LOGW("pair-verify alone was rejected; pairing again");
    }

    if (session->config->pin != NULL && session->config->pin[0] != '\0') {
        pin = session->config->pin;
        session->hkp = session->prefers_legacy_pairing ? "3" : "5";
    } else if (want_pin) {
        pin = request_code(session);
        if (pin == NULL) {
            AP_LOGE("receiver requires a pairing code and none was supplied");
            return ESP_FAIL;
        }
    } else {
        session->hkp = "4";
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
    /* Apple leaves the port out of the URI even when the receiver listens on a
     * non-default one; a receiver that matches the URL strictly rejects the
     * request as soon as the port is present. */
    int uri_len = snprintf(uri, sizeof(uri), "rtsp://%s/%llu", session->config->host,
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
    size_t largest = 0;
    if (session->config->live_source != NULL) {
        /* A live source has no enumerable frames; it publishes its bound. */
        largest = session->config->live_source->max_au_len;
        if (largest == 0) {
            AP_LOGE("live source publishes no access-unit bound");
            return ESP_FAIL;
        }
    } else {
        const airplay_frames_t *frames = session->config->frames;
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
    }

    /* One allocation for the sealed access unit: ChaCha20-Poly1305 output
     * cannot alias the flash-resident plaintext. Sized once to the largest
     * frame, never per frame. */
    /* Legacy sync frames carry the SPS/PPS in-band, so leave room for them. */
    session->frame_scratch_len = largest + AP_FRAME_TAG_LEN + 128;
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
    const airplay_live_source_t *live = session->config->live_source;
    const airplay_frames_t *frames = session->config->frames;
    const bool legacy = session->config->legacy;
    /* interval_ms == 0 tells the loop to run at whatever pace the source
     * delivers, which is what a capture client wants: its own frames already
     * arrive at the screen's update rate, and adding a nominal interval on top
     * would only add latency and cap the rate twice. */
    uint32_t interval = live != NULL ? live->interval_ms : session->config->frame_interval_ms;
    if (interval == 0 && live == NULL) {
        interval = 1000;
    }

    const uint8_t *avcc = NULL;
    size_t avcc_len = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    if (live != NULL) {
        avcc = live->codec_record(live->user, &avcc_len);
        width = live->width;
        height = live->height;
    } else {
        avcc = frames->avcc;
        avcc_len = frames->avcc_len;
        width = frames->width;
        height = frames->height;
    }
    if (avcc == NULL || avcc_len == 0 || width == 0 || height == 0) {
        AP_LOGE("content source carries no usable codec record");
        return ESP_FAIL;
    }
    AP_LOGI("video source selected: %s, %ux%u",
            live != NULL ? (live->name != NULL ? live->name : "live source") : "still frames",
            (unsigned)width, (unsigned)height);

    uint64_t timeline_id = 0;
    /* Set by the codec record so the first frame can share its timestamp. */
    uint64_t legacy_codec_timestamp = 0;
    uint8_t header[AP_FRAME_HEADER_LEN];
    if (legacy) {
        /* The codec record rides unencrypted with the raster dimensions in the
         * header floats (RPiPlay lib/raop_rtp_mirror.c:350-375). Its timestamp
         * is the one the first frame will carry: a receiver that anchors the
         * decoder on the codec record drops the stream when the two disagree. */
        legacy_codec_timestamp = ap_ntp_boot_timestamp();
        ap_frame_build_legacy(header, avcc_len, 0x01, legacy_codec_timestamp, width, height, false);
        if (ap_legacy_write_record(session->data.fd,
                                   session->legacy_transport == 1 || session->legacy_transport == 3,
                                   0, header, avcc, avcc_len) != 0) {
            AP_LOGE("legacy codec record write failed");
            return ESP_FAIL;
        }
    } else if (ap_data_send_codec(&session->data, avcc, avcc_len, width, height,
                                  ap_clock_now(&session->clock, &timeline_id)) != 0) {
        AP_LOGE("codec packet write failed");
        return ESP_FAIL;
    }
    AP_LOGI("codec packet sent: avcC %u bytes, %ux%u", (unsigned)avcc_len, (unsigned)width,
            (unsigned)height);

    size_t index = 0;
    /* The first video record repeats the codec record's timestamp, as Apple's
     * sender does, so the comparison has to start one tick below it. */
    uint64_t last_timestamp = legacy_codec_timestamp != 0 ? legacy_codec_timestamp - 1 : 0;
    uint64_t next_frame = ap_millis();
    uint64_t next_heartbeat = 0;
    uint64_t next_feedback = ap_millis();
    bool first_frame_sent = false;
    int feedback_failures = 0;
    int feedback_deferrals = 0;
    uint32_t frames_sent = 0;
    uint64_t last_frame_at = 0;
    esp_err_t result = ESP_OK;

    while (!session->stop && !s_stop_requested) {
        uint64_t now = ap_millis();
        uint64_t slack = next_frame > now ? next_frame - now : 0;
        /* A source with no nominal interval (a capture client) has no future
         * deadline to look at, so idle time since the last frame serves as the
         * window for control traffic; otherwise feedback and heartbeats would
         * never find room and the receiver would consider the channel idle. */
        bool control_slot = slack >= AP_SLACK_FOR_CONTROL_MS ||
                            (interval == 0 && now >= last_frame_at + AP_SLACK_FOR_CONTROL_MS);

        if (now >= next_feedback && (control_slot || feedback_deferrals >= AP_FEEDBACK_MAX_DEFERRALS)) {
            feedback_deferrals = 0;
            if (send_feedback(session) == 0) {
                feedback_failures = 0;
            } else if (++feedback_failures >= AP_FEEDBACK_FAILURE_LIMIT) {
                AP_LOGE("feedback failed %d times in a row; ending the session", feedback_failures);
                result = ESP_FAIL;
                break;
            }
            next_feedback = now + AP_FEEDBACK_INTERVAL_MS;
            continue;
        }
        if (now >= next_feedback) {
            feedback_deferrals++;
        }

        if (!legacy && first_frame_sent && now >= next_heartbeat && control_slot) {
            if (ap_data_send_heartbeat(&session->data) != 0) {
                AP_LOGE("data channel heartbeat failed");
                result = ESP_FAIL;
                break;
            }
            next_heartbeat = now + AP_HEARTBEAT_INTERVAL_MS;
        }

        if (now >= next_frame) {
            size_t payload_len = 0;
            const uint8_t *payload = NULL;
            bool idr = false;
            uint64_t frame_started = ap_millis();
            if (live != NULL) {
                esp_err_t produced = live->next_frame(live->user, &payload, &payload_len, &idr);
                if (produced == ESP_ERR_TIMEOUT) {
                    /* A live source may briefly run dry; skip this slot instead
                     * of tearing down a session that is still healthy. */
                    next_frame = now + interval;
                    continue;
                }
                if (produced != ESP_OK || payload == NULL || payload_len == 0) {
                    AP_LOGE("live source failed to produce a frame");
                    result = ESP_FAIL;
                    break;
                }
            } else {
                payload = frames->frame(index, &payload_len);
                if (payload == NULL || payload_len == 0) {
                    AP_LOGE("still frame %u disappeared", (unsigned)index);
                    return ESP_FAIL;
                }
                idr = ap_avcc_has_idr(payload, payload_len);
            }

            uint64_t timestamp = legacy ? ap_ntp_boot_timestamp() : ap_clock_now(&session->clock, &timeline_id);
            if (timestamp <= last_timestamp) {
                timestamp = last_timestamp + 1;
            }
            last_timestamp = timestamp;

            uint64_t wait_ms = ap_millis() - frame_started;
            if (legacy) {
                /* The whole access unit is sealed with the continuous CTR
                 * keystream; no tag, and the keystream position carries over
                 * to the next record. */
                if (payload_len > session->frame_scratch_len) {
                    AP_LOGE("frame %u does not fit the scratch buffer", (unsigned)index);
                    return ESP_FAIL;
                }
                /* Hardware decoders that ignore the out-of-band avcC need the
                 * parameter sets in-band with every sync frame. */
                if (idr && session->config->legacy_inband_sps) {
                    size_t assembled = ap_frame_add_parameter_sets(
                        session->frame_scratch, session->frame_scratch_len, avcc, avcc_len, payload,
                        payload_len);
                    if (assembled == 0) {
                        AP_LOGE("frame plus parameter sets does not fit", (unsigned)index);
                        return ESP_FAIL;
                    }
                    payload_len = assembled;
                } else {
                    memcpy(session->frame_scratch, payload, payload_len);
                }
                ap_legacy_ctr_xor(&session->legacy_ctr, session->frame_scratch, payload_len);
                ap_frame_build_legacy(header, payload_len, 0x00, timestamp, 0, 0, idr);
                if (ap_legacy_write_record(session->data.fd,
                                           session->legacy_transport == 1 ||
                                               session->legacy_transport == 3,
                                           0, header, session->frame_scratch, payload_len) != 0) {
                    AP_LOGE("video frame write failed");
                    result = ESP_FAIL;
                    break;
                }
            } else if (ap_data_send_frame(&session->data, payload, payload_len, idr, timestamp,
                                          timeline_id, session->frame_scratch,
                                          session->frame_scratch_len) != 0) {
                AP_LOGE("video frame write failed");
                result = ESP_FAIL;
                break;
            }
            if (!first_frame_sent) {
                first_frame_sent = true;
                next_heartbeat = ap_millis() + AP_HEARTBEAT_INTERVAL_MS;
            }
            frames_sent++;
            s_streamed = true;
            last_frame_at = ap_millis();
            /* One line per frame would block on the USB console often enough to
             * throttle the stream itself, so a live source reports once a second
             * and still frames (10 s apart) report every frame. */
            uint32_t log_every = live != NULL ? (interval > 0 ? (1000u / interval) : 10u) : 1u;
            if (log_every < 1) {
                log_every = 1;
            }
            /* Every sync frame is worth a line: a receiver that never gets one
             * mid-stream keeps showing a stale picture, so the IDR line is the
             * first thing to look for when the screen stays black. */
            if (frames_sent == 1 || frames_sent % log_every == 0 || idr) {
                AP_LOGI("frame %u sent: %u bytes, idr=%d, waited=%llums, sent in %llums",
                        (unsigned)frames_sent, (unsigned)payload_len, idr ? 1 : 0,
                        (unsigned long long)wait_ms,
                        (unsigned long long)(ap_millis() - frame_started - wait_ms));
            }

            if (live == NULL) {
                index = (index + 1) % frames->frame_count;
            }
            /* Accumulate instead of restarting from "now": a send that takes a
             * little longer than the interval would otherwise push every later
             * frame back and the stream would drift below the target rate. */
            uint64_t deadline = next_frame + interval;
            if (deadline <= now) {
                /* Fell more than one interval behind; drop the backlog rather
                 * than burst frames at the receiver. */
                deadline = now + interval;
            }
            next_frame = deadline;
        }

        vTaskDelay(pdMS_TO_TICKS(AP_LOOP_TICK_MS));
    }

    return result;
}

/* Legacy (AirTunes) front half: raw pair, FairPlay ekey chain, ANNOUNCE and
 * the two SETUPs. Returns the receiver's mirror data port. The NTP responder
 * is running when this returns - SETUP 1 hands out its port. */
static esp_err_t legacy_front_half(ap_session_t *session, uint16_t *data_port)
{
    ap_response_t response;
    if (ap_client_request(&session->client, "OPTIONS", "*", NULL, NULL, 0, NULL, &response) != 0) {
        AP_LOGE("legacy OPTIONS failed (status %d)", response.status);
        return ESP_FAIL;
    }
    if (response.is_http) {
        /* Boxes of this class advertise an AirTunes HTTP stub (the port mDNS
         * reports) and keep the real RTSP service on the next port up. Nothing
         * useful happens on the stub: it answers 200 to everything and never
         * streams, so step up once and carry on there. */
        uint16_t port = (uint16_t)(session->control_port + 1);
        AP_LOGW("port %u answers HTTP; retrying the RTSP service on %u",
                (unsigned)session->control_port, (unsigned)port);
        ap_client_close(&session->client);
        if (ap_client_connect(&session->client, session->config->host, port,
                              AP_CONTROL_CONNECT_TIMEOUT_MS) != 0) {
            AP_LOGE("cannot connect to %s:%u", session->config->host, (unsigned)port);
            return ESP_FAIL;
        }
        if (ap_client_request(&session->client, "OPTIONS", "*", NULL, NULL, 0, NULL, &response) != 0 ||
            response.is_http) {
            AP_LOGE("no RTSP service on %s:%u either", session->config->host, (unsigned)port);
            return ESP_FAIL;
        }
        session->control_port = port;
    }

    /* Apple's own screen mirroring skips pairing on this class of receiver and
     * lets FairPlay carry the key material, so pairing is optional. */
    if (session->config->legacy_pairing) {
        uint8_t receiver_ed[32];
        if (ap_legacy_pair_setup(&session->client, session->identity, receiver_ed) != 0 ||
            ap_legacy_pair_verify(&session->client, session->identity, receiver_ed,
                                  session->shared) != 0) {
            return ESP_FAIL;
        }
        AP_LOGI("legacy pair-verify complete");
    } else {
        AP_LOGI("legacy session without pairing, as Apple sends it");
    }

    /* FairPlay v3 with the ekey chain: the same primitives the HAP path uses,
     * but the wrapped key travels in SETUP 1 instead of shk over HAP. */
    if (fp_session_init(&session->fp, fairplay_random, NULL) != 0) {
        AP_LOGE("FairPlay: session init failed");
        return ESP_FAIL;
    }
    uint8_t m1[FP_M1_LEN];
    fp_message1(&session->fp, m1);
    if (ap_client_request(&session->client, "POST", "/fp-setup", AP_CONTENT_TYPE_OCTET, m1,
                          sizeof(m1), NULL, &response) != 0) {
        AP_LOGE("legacy FairPlay: m1 rejected");
        return ESP_FAIL;
    }
    size_t body_len = 0;
    const uint8_t *m2 = ap_client_body(&session->client, &body_len);
    ap_random_bytes(session->aeskey, sizeof(session->aeskey));
    ap_random_bytes(session->aesiv, sizeof(session->aesiv));

    /* Third-party boxes often stub FairPlay out: m1 is answered with an empty
     * body. There is no ekey to wrap in that case, the bare AES key is the
     * stream key, and SETUP 1 goes out without the ekey field. */
    bool fairplay = body_len != 0;
    if (fairplay) {
        uint8_t m3[FP_M3_LEN];
        if (fp_exchange_m3(&session->fp, m2, body_len, m3) != 0 ||
            ap_client_request(&session->client, "POST", "/fp-setup", AP_CONTENT_TYPE_OCTET, m3,
                              sizeof(m3), NULL, &response) != 0) {
            AP_LOGE("legacy FairPlay: m3 rejected");
            return ESP_FAIL;
        }
        const uint8_t *m4 = ap_client_body(&session->client, &body_len);
        if (fp_confirm_m4(&session->fp, m4, body_len) != 0) {
            AP_LOGE("legacy FairPlay: m4 confirmation failed");
            return ESP_FAIL;
        }
        if (fp_wrap_key(&session->fp, session->aeskey, fairplay_random, NULL, session->ekey) != 0) {
            AP_LOGE("legacy FairPlay: ekey wrapping failed");
            return ESP_FAIL;
        }
        AP_LOGI("legacy FairPlay handshake complete (ekey wrapped)");
    } else {
        AP_LOGW("receiver answered fp-setup with no body; continuing without FairPlay");
    }

    /* SETUP 1 hands out our timing port, so the responder must listen first. */
    ap_clock_use_ntp(&session->clock);
    session->timing_fd = ap_ntp_open(&session->timing_port);
    if (session->timing_fd < 0) {
        AP_LOGE("could not bind the NTP timing port");
        return ESP_FAIL;
    }
    /* SETUP 2 declares this as the audio controlPort: the receiver's RTCP
     * receiver reports land there, and they are the only view of what actually
     * arrived at the speaker. */
    session->audio_control_fd = ap_ntp_open(&session->audio_control_port);
    if (session->audio_control_fd < 0) {
        AP_LOGW("could not bind the audio control port; no RTCP reports");
    }
    if (ap_ntp_start(session) != 0) {
        return ESP_FAIL;
    }

    char announce[512];
    size_t announce_len = ap_legacy_build_announce(
        announce, sizeof(announce), session->session_uuid,
        session->config->local_address != NULL ? session->config->local_address : "0.0.0.0",
        session->config->host);
    if (ap_client_request(&session->client, "ANNOUNCE", session->uri, "application/sdp", announce,
                          announce_len, NULL, &response) != 0) {
        AP_LOGE("legacy ANNOUNCE failed (status %d)", response.status);
        return ESP_FAIL;
    }

    uint8_t plist[1024];
    char correlation_uuid[37];
    {
        uint8_t bytes[16];
        ap_random_bytes(bytes, sizeof(bytes));
        format_uuid(bytes, correlation_uuid);
    }
    ap_legacy_setup1_t setup1 = {
        .eiv = session->aesiv,
        .ekey = fairplay ? session->ekey : NULL,
        .timing_port = session->timing_port,
        .session_uuid = session->session_uuid,
        .session_correlation_uuid = correlation_uuid,
        .device_id = session->device_id,
        .mac_address = session->device_id,
    };
    size_t plist_len = ap_legacy_build_setup1(plist, sizeof(plist), &setup1);
    if (plist_len == 0 ||
        ap_client_request(&session->client, "SETUP", session->uri, AP_CONTENT_TYPE_PLIST, plist,
                          plist_len, NULL, &response) != 0) {
        AP_LOGE("legacy SETUP 1 failed (status %d)", response.status);
        return ESP_FAIL;
    }
    AP_LOGI("legacy SETUP 1: timingPort=%u", (unsigned)session->timing_port);

    /* Apple queries /info and the volume between the two SETUPs. */
    (void)ap_client_request(&session->client, "GET", "/info", NULL, NULL, 0, NULL, &response);
    (void)ap_client_request(&session->client, "GET_PARAMETER", session->uri, "text/parameters",
                            "volume", 6, NULL, &response);

    /* Apple sends no Transport header with the mirror SETUP: the receiver
     * either hands out a dataPort of its own or keeps the stream on this
     * connection. Offering "interleaved=0-1" here makes receivers answer
     * without a dataPort and then expect a framing the sender never agreed
     * to, so the request matches Apple's byte for byte. */
    ap_header_t transport[2] = {
        { "X-Apple-ProtocolVersion", "1" },
        { NULL, NULL },
    };
    /* Apple sends RECORD before the mirror SETUP; some receivers start their
     * decoder only once the session is recording. */
    {
        ap_header_t record_headers[4] = {
            { "Session", session->session_uuid },
            { "Range", "npt=0-" },
            { "RTP-Info", "seq=0;rtptime=0" },
            { NULL, NULL },
        };
        if (ap_client_request(&session->client, "RECORD", session->uri, NULL, NULL, 0,
                              record_headers, &response) != 0) {
            AP_LOGE("legacy RECORD failed (status %d)", response.status);
            return ESP_FAIL;
        }
        session->legacy_recorded = true;
    }

    /* Some receivers keep the advertised port as an RTSP stub and answer the
     * mirror SETUP on a second port; that port allocates the data channel. */
    ap_client_t *stream_client = &session->client;
    uint16_t stream_port = session->config->legacy_stream_port;
    if (stream_port != 0 && stream_port != session->control_port) {
        if (ap_client_connect(&session->stream_client, session->config->host, stream_port,
                              AP_CONTROL_CONNECT_TIMEOUT_MS) != 0) {
            AP_LOGE("cannot reach the mirror port %u", (unsigned)stream_port);
            return ESP_FAIL;
        }
        session->stream_client_open = true;
        stream_client = &session->stream_client;
        AP_LOGI("legacy SETUP 2 goes to port %u", (unsigned)stream_port);
    }
    bool with_audio = session->config->legacy_audio && session->config->legacy_audio_spf > 0;
    plist_len = ap_legacy_build_setup2(plist, sizeof(plist), session->video_stream_id, with_audio,
                                       session->audio_control_port,
                                       session->config->legacy_audio_spf);
    if (plist_len == 0 ||
        ap_client_request(stream_client, "SETUP", session->uri, AP_CONTENT_TYPE_PLIST, plist,
                          plist_len, transport, &response) != 0) {
        AP_LOGE("legacy SETUP 2 failed (status %d)", response.status);
        return ESP_FAIL;
    }

    body_len = 0;
    const uint8_t *body = ap_client_body(stream_client, &body_len);
    ap_plist_reader_t reader;
    uint64_t streams = 0;
    uint64_t stream = 0;
    uint64_t port = 0;
    bool has_port = body_len != 0 && ap_plist_decode(&reader, body, body_len) &&
                    ap_plist_dict_get(&reader, ap_plist_root(&reader), "streams", &streams) &&
                    ap_plist_array_get(&reader, streams, 0, &stream) &&
                    ap_plist_dict_uint(&reader, stream, "dataPort", &port) && port != 0 &&
                    port <= 65535;
    if (with_audio) {
        uint64_t audio_stream = 0;
        uint64_t audio_port = 0;
        if (ap_plist_array_get(&reader, streams, 1, &audio_stream) &&
            ap_plist_dict_uint(&reader, audio_stream, "dataPort", &audio_port) && audio_port != 0 &&
            audio_port <= 65535) {
            session->legacy_audio_port = (uint16_t)audio_port;
            AP_LOGI("legacy SETUP 2: audio dataPort=%u", (unsigned)session->legacy_audio_port);
        } else {
            AP_LOGW("legacy SETUP 2: the receiver allocated no audio port");
        }
    }
    if (has_port) {
        *data_port = (uint16_t)port;
        session->legacy_transport = 0;
        AP_LOGI("legacy SETUP 2: dataPort=%u", (unsigned)*data_port);
    } else {
        session->legacy_transport = session->config->legacy_data_mode;
        if (session->legacy_transport < 1 || session->legacy_transport > 4) {
            session->legacy_transport = 2;
        }
        *data_port = 0;
        AP_LOGI("legacy SETUP 2: no dataPort; transport mode %u (%s on the %s connection)",
                (unsigned)session->legacy_transport,
                session->legacy_transport == 1 || session->legacy_transport == 3 ? "interleaved"
                                                                               : "raw records",
                session->legacy_transport <= 2 ? "control" : "second");
    }

    /* Without FairPlay there is no receiver-side mix step either, so the
     * stream keys come from the bare AES key. */
    uint8_t mode = fairplay ? session->config->legacy_key_mode : 0;
    ap_legacy_derive_stream_keys(session->aeskey, session->shared, mode,
                                 session->video_stream_id, session->legacy_skey,
                                 session->legacy_siv);
    if (mode == 3 || session->config->legacy_iv_source == 1) {
        memcpy(session->legacy_siv, session->aesiv, sizeof(session->legacy_siv));
    }
    return ESP_OK;
}

/* The audio channel gets its own task: writing one video record takes tens of
 * milliseconds, and audio queued behind it reaches the receiver in bursts that
 * its buffer renders as clicks. */
static volatile bool s_audio_stop;
static TaskHandle_t s_audio_task;

/* The receiver answers the audio controlPort with RTCP, and the receiver
 * report says what no local counter can: how many of the packets we sent
 * actually survived the air. A fraction lost that tracks the video keyframes
 * means the mirror stream pushes the audio datagrams out of the receiver's
 * jitter budget; a clean report with noise in the speaker points at the decode
 * or the receiver itself. */
static void audio_rtcp_poll(ap_session_t *session)
{
    static uint64_t last_log_ms;
    static uint32_t reports;
    uint8_t packet[272];

    if (session->audio_control_fd < 0) {
        return;
    }
    for (;;) {
        int len = recvfrom(session->audio_control_fd, packet, sizeof(packet), MSG_DONTWAIT, NULL, NULL);
        if (len < 8) {
            return;
        }
        int offset = 0;
        while (offset + 8 <= len) {
            uint8_t pt = packet[offset + 1];
            uint16_t words = (uint16_t)(((uint16_t)packet[offset + 2] << 8) | packet[offset + 3]);
            int size = ((int)words + 1) * 4;
            if (size < 8 || offset + size > len) {
                return;
            }
            if (pt == 200 || pt == 201) {
                /* Report blocks follow the 8-byte header, plus the 20-byte
                 * sender info of a sender report. */
                int block = offset + 8 + (pt == 200 ? 20 : 0);
                int count = packet[offset] & 0x1f;
                for (int i = 0; i < count && block + 24 <= offset + size; i++, block += 24) {
                    const uint8_t *r = packet + block;
                    uint32_t cumulative = ((uint32_t)r[5] << 16) | ((uint32_t)r[6] << 8) | r[7];
                    uint32_t jitter = ((uint32_t)r[12] << 24) | ((uint32_t)r[13] << 16)
                                        | ((uint32_t)r[14] << 8) | r[15];
                    reports++;
                    if (reports == 1 || ap_millis() - last_log_ms >= 5000) {
                        AP_LOGI("audio RTCP: fraction lost %.1f%%, cumulative %u, jitter %u",
                                r[4] / 2.55, (unsigned)cumulative, (unsigned)jitter);
                        last_log_ms = ap_millis();
                    }
                }
            }
            offset += size;
        }
    }
}

static void audio_task(void *arg)
{
    ap_session_t *session = (ap_session_t *)arg;
    const uint32_t spf = session->config->legacy_audio_spf;
    uint8_t *packet = malloc(12 + AP_AUDIO_FRAME_MAX);
    if (packet == NULL) {
        AP_LOGE("no memory for the audio packet buffer");
        s_audio_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    uint64_t started = ap_millis();
    uint64_t index = 0;
    while (!s_audio_stop && !session->stop && !s_stop_requested && session->legacy_audio_fd >= 0) {
        uint64_t due = started + index * spf * 1000 / AP_LEGACY_AUDIO_RATE;
        uint64_t now = ap_millis();
        if (now < due) {
            uint64_t wait = due - now;
            vTaskDelay(pdMS_TO_TICKS(wait < 20 ? wait : 20) + 1);
            continue;
        }
        uint64_t late = now - due;
        if (late > session->legacy_audio_late_max) {
            session->legacy_audio_late_max = late;
        }
        if (late > 20) {
            session->legacy_audio_late_count++;
        }
        ap_audio_frame_t frame = { 0 };
        size_t frame_len = session->config->legacy_audio_frame == NULL
                               ? 0
                               : session->config->legacy_audio_frame(
                                     session->config->legacy_audio_ctx, &frame);
        int len = 0;
        if (frame_len > 0 && frame_len <= AP_AUDIO_FRAME_MAX) {
            len = ap_legacy_build_audio(packet, 12 + AP_AUDIO_FRAME_MAX, session->legacy_audio_seq,
                                        session->legacy_audio_rtp_time, frame.data, frame_len,
                                        session->legacy_audio_keys[frame.mixed_key ? 1 : 0],
                                        session->aesiv);
        } else if (frame_len > AP_AUDIO_FRAME_MAX) {
            AP_LOGW("audio frame of %u bytes is over the %u-byte limit",
                    (unsigned)frame_len, (unsigned)AP_AUDIO_FRAME_MAX);
        }
        if (len == 0) {
            /* Nothing to send: the receiver plays silence for this slot. */
            session->legacy_audio_gaps++;
        }
        session->legacy_audio_seq++;
        session->legacy_audio_rtp_time += spf;
        session->legacy_audio_packets++;
        index++;
        if (len > 0 && ap_socket_send_to(session->legacy_audio_fd, session->config->host,
                                         session->legacy_audio_port, packet, (size_t)len) != 0) {
            AP_LOGW("legacy audio write failed; closing the audio channel");
            ap_socket_close(session->legacy_audio_fd);
            session->legacy_audio_fd = -1;
            break;
        }
        audio_rtcp_poll(session);
    }
    free(packet);
    s_audio_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t session_run(ap_session_t *session)
{
    session->control_port = session->config->port;
    if (ap_client_connect(&session->client, session->config->host, session->control_port,
                          AP_CONTROL_CONNECT_TIMEOUT_MS) != 0) {
        AP_LOGE("cannot connect to %s:%u", session->config->host, (unsigned)session->control_port);
        return ESP_FAIL;
    }
    /* Receivers that protect the session with "Require Password" answer with a
     * Digest challenge, and the value they expect is the same code the user
     * typed for pairing. */
    if (session->config->pin != NULL && session->config->pin[0] != '\0') {
        ap_client_set_password(&session->client, session->config->pin);
    }
    if (session->config->legacy) {
        char line[64];
        snprintf(line, sizeof(line), "DACP-ID: %016llX",
                 (unsigned long long)(ap_millis() * 2654435761u));
        ap_client_set_extra_header(&session->client, 0, line);
        snprintf(line, sizeof(line), "Active-Remote: %u", (unsigned)(ap_millis() & 0xffffffffu));
        ap_client_set_extra_header(&session->client, 1, line);
    }

    uint16_t data_port = 0;
    if (session->config->legacy) {
        if (legacy_front_half(session, &data_port) != ESP_OK) {
            return ESP_FAIL;
        }
        if (session->legacy_transport >= 1 && session->legacy_transport <= 2) {
            /* The stream rides the control connection itself. */
            session->data.fd = session->client.fd;
        } else {
            uint16_t connect_port =
                session->legacy_transport >= 3 ? session->control_port : data_port;
            session->data.fd =
                ap_socket_connect(session->config->host, connect_port, AP_DATA_CONNECT_TIMEOUT_MS);
            if (session->data.fd < 0) {
                AP_LOGE("cannot connect to the legacy data port %u", (unsigned)connect_port);
                return ESP_FAIL;
            }
        }
        ap_legacy_ctr_start(&session->legacy_ctr, session->legacy_skey, session->legacy_siv);
        AP_LOGI("legacy data channel ready on fd %d", session->data.fd);

        /* The audio channel rides UDP to the port the receiver allocated. */
        if (session->config->legacy_audio && session->legacy_audio_port != 0) {
            uint16_t local_port = 0;
            session->legacy_audio_fd = ap_socket_udp_open(&local_port);
            if (session->legacy_audio_fd < 0) {
                AP_LOGW("cannot open the audio socket");
            } else {
                ap_legacy_audio_keys(session->aeskey, session->legacy_audio_keys[0],
                                     session->legacy_audio_keys[1]);
                session->legacy_audio_rtp_time =
                    (uint32_t)((ap_ntp_boot_timestamp() >> 32) * AP_LEGACY_AUDIO_RATE);
                session->legacy_audio_started_ms = ap_millis();
                session->legacy_audio_seq = 0;
                session->legacy_audio_packets = 0;
                AP_LOGI("legacy audio channel ready: local %u -> %s:%u, %u samples per packet",
                        (unsigned)local_port, session->config->host,
                        (unsigned)session->legacy_audio_port,
                        (unsigned)session->config->legacy_audio_spf);
                s_audio_stop = false;
                if (xTaskCreate(audio_task, "ap_audio", 4096, session, 6, &s_audio_task) != pdPASS) {
                    AP_LOGE("cannot start the audio task");
                    s_audio_task = NULL;
                }
            }
        }
    } else {
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
    }

    if (!session->config->legacy && !session->skip_record) {
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

    s_audio_stop = true;
    for (int i = 0; i < 100 && s_audio_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (session->config->legacy_audio) {
        AP_LOGI("legacy audio: %llu packets, %llu without a frame, %llu late (max %llu ms)",
                (unsigned long long)session->legacy_audio_packets,
                (unsigned long long)session->legacy_audio_gaps,
                (unsigned long long)session->legacy_audio_late_count,
                (unsigned long long)session->legacy_audio_late_max);
    }

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
    session->legacy_audio_fd = -1;
    }
    ap_data_close(&session->data);

    /* Give the NTP and event tasks a moment to observe the stop flag before the
     * control connection goes away. */
    vTaskDelay(pdMS_TO_TICKS(100));

    if (session->stream_client_open) {
        ap_client_close(&session->stream_client);
        session->stream_client_open = false;
    }
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
    if (config == NULL || config->host == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->live_source != NULL) {
        if (config->frames != NULL || config->live_source->codec_record == NULL ||
            config->live_source->next_frame == NULL || config->live_source->max_au_len == 0 ||
            config->live_source->width == 0 || config->live_source->height == 0) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (config->frames == NULL || config->frames->frame == NULL ||
               config->frames->frame_count == 0 || config->frames->avcc == NULL ||
               config->frames->avcc_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_mirroring = true;
    s_stop_requested = false;
    s_streamed = false;

    ap_session_t *session = &s_session;
    memset(session, 0, sizeof(*session));
    session->config = config;
    session->client.fd = -1;
    session->data.fd = -1;
    session->event.fd = -1;
    session->timing_fd = -1;
    session->audio_control_fd = -1;
    session->legacy_audio_fd = -1;

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
    /* See the note on the video URI: Apple omits the port here too. */
    int uri_len = snprintf(session->uri, sizeof(session->uri), "rtsp://%s/%llu", config->host,
                           (unsigned long long)session->audio_stream_id);
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

    AP_LOGI("mirroring to %s:%u as \"%s\" (%s, %s)", config->host, (unsigned)config->port,
            session->sender_name, session->device_id,
            config->legacy ? "legacy AirTunes" : "HAP");
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
    s_mirroring = false;
    return result;
}

void airplay_request_stop(void)
{
    s_stop_requested = true;
}

void airplay_last_audio_stats(airplay_audio_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    stats->packets = s_session.legacy_audio_packets;
    stats->gaps = s_session.legacy_audio_gaps;
    stats->late = s_session.legacy_audio_late_count;
    stats->late_max_ms = s_session.legacy_audio_late_max;
}

bool airplay_last_session_streamed(void)
{
    return s_streamed;
}

bool airplay_is_running(void)
{
    return s_mirroring;
}
