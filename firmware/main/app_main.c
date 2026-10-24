#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "airplay.h"
#include "app_config.h"
#include "app_identity.h"
#include "discovery.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "fairplay.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media_frames.h"
#include "media_live.h"
#include "media_net.h"
#include "media_text.h"
#include "nvs_flash.h"
#include "pin_fetch.h"
#include "pin_server.h"
#include "tone.h"
#include "wifi_net.h"

static const char *TAG = "app";

/* Session control state, touched from the HTTP server task and the session
 * task. Single-writer volatile bools are enough: the readers only need the
 * value eventually. */
static volatile bool s_session_stop;
static char s_last_result[80];
static TaskHandle_t s_session_task;
static airplay_config_t s_config;
/* The session task outlives app_main, so everything the config points at must
 * not live on app_main's stack. */
static hap_identity_t s_identity;
static char s_local_ip[16];
/* The target lives in a writable buffer so /start can aim the session at a
 * discovered receiver; it is only written while no session is running. */
static char s_target_host[48];
/* Name this receiver is known by in the list: the mDNS name when one was used,
 * otherwise the address the session was aimed at. */
static char s_target_name[DISCOVERY_NAME_LEN];
/* Replies assembled for /start, including the list of names seen when a
 * requested receiver is not found. */
static char s_reply[512];

/* Content source for the session that is about to start. A connected capture
 * client wins over the on-chip test pattern, which in turn wins over the
 * flash-resident still images. The choice is made per session because a client
 * may connect or drop between sessions. */
static void select_video_source(void)
{
    const airplay_live_source_t *live = NULL;
#if APP_USE_NETWORK_VIDEO
    live = media_net_source();
#endif
#if APP_USE_LIVE_VIDEO
    if (live == NULL) {
        live = media_live_source();
    }
#endif
    s_config.live_source = live;
    s_config.frames = live == NULL ? media_still_frames() : NULL;
    ESP_LOGI(TAG, "video source: %s",
             live != NULL ? (live->name != NULL ? live->name : "live source") : "still images");
}

static void session_task(void *argument)
{
    (void)argument;
    snprintf(s_last_result, sizeof(s_last_result), "idle since boot");
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_session_stop) {
            /* /stop arrived before the session even started. */
            s_session_stop = false;
            snprintf(s_last_result, sizeof(s_last_result), "stopped before it started");
            continue;
        }
        select_video_source();
        ESP_LOGI(TAG, "starting a mirroring session on request");
        esp_err_t err = airplay_mirror(&s_config);
        snprintf(s_last_result, sizeof(s_last_result),
                 err == ESP_OK ? "last session: ok" : "last session: %s",
                 err == ESP_OK ? "" : esp_err_to_name(err));
        if (err == ESP_OK || airplay_last_session_streamed()) {
            /* A receiver that never answers a browse announcement is only ever
             * seen through a working session. */
            discovery_remember(s_target_name, s_target_host, s_config.port, s_config.legacy);
        }
        /* The session ends on failure, on /stop, or when the receiver goes
         * away. Nothing restarts it automatically: /start does that. */
        s_session_stop = false;
        ESP_LOGI(TAG, "%s", s_last_result);
    }
}

/* Which stream-key candidate the audio channel uses: the bare key (what the
 * video path uses on this class of receiver) or the one mixed with an all-zero
 * secret. */
static bool s_audio_mixed;

/* One ALAC frame per call, straight from the capture client. An empty queue
 * returns nothing, which keeps the timeline moving without inventing audio. */
static size_t network_audio_frame(void *ctx, ap_audio_frame_t *frame)
{
    (void)ctx;
    size_t len = 0;
    const uint8_t *data = media_net_audio(&len);
    if (data == NULL || len == 0) {
        return 0;
    }
    frame->data = data;
    frame->len = len;
    frame->mixed_key = s_audio_mixed;
    return len;
}

static const char *hook_start(const pin_start_request_t *request)
{
    if (airplay_is_running()) {
        return "already mirroring\n";
    }

    /* Per-session overrides let a receiver be probed without reflashing. Each
     * session starts from the compiled configuration, so an override from an
     * earlier /start cannot leak into this one. */
    s_config.legacy_data_mode = APP_LEGACY_DATA_MODE;
    s_config.legacy_key_mode = (uint8_t)APP_LEGACY_KEY_MIX;
    s_config.legacy_iv_source = APP_LEGACY_IV_SOURCE;
    s_config.legacy_stream_port = APP_LEGACY_STREAM_PORT;
    s_config.legacy_pairing = APP_LEGACY_PAIRING != 0;
    s_config.legacy_inband_sps = APP_LEGACY_INBAND_SPS != 0;
    s_config.frame_interval_ms = APP_FRAME_INTERVAL_MS;
    s_config.legacy_audio = false;
    s_config.legacy_audio_spf = 0;
    s_config.legacy_audio_frame = NULL;
    s_config.legacy_audio_ctx = NULL;
    if (request->data_mode >= 0) {
        s_config.legacy_data_mode = (uint8_t)request->data_mode;
    }
    if (request->key_mix >= 0) {
        s_config.legacy_key_mode = (uint8_t)request->key_mix;
    }
    if (request->iv_source >= 0) {
        s_config.legacy_iv_source = (uint8_t)request->iv_source;
    }
    if (request->stream_port >= 0) {
        s_config.legacy_stream_port = (uint16_t)request->stream_port;
    }
    if (request->pair >= 0) {
        s_config.legacy_pairing = request->pair != 0;
    }
    if (request->sps >= 0) {
        s_config.legacy_inband_sps = request->sps != 0;
    }
    s_audio_mixed = request->amix > 0;
    if (request->audio == 1) {
        /* Real audio: the capture client sends ALAC frames alongside the video. */
        s_config.legacy_audio = true;
        s_config.legacy_audio_spf = APP_AUDIO_SPF;
        s_config.legacy_audio_frame = network_audio_frame;
    } else if (request->audio == 2) {
        /* Test tone: tells the two stream-key candidates apart by ear. */
        legacy_tone_reset();
        s_config.legacy_audio = true;
        s_config.legacy_audio_spf = TONE_FRAME_SAMPLES;
        s_config.legacy_audio_frame = legacy_tone_frame;
    }
    if (request->interval_ms > 0) {
        s_config.frame_interval_ms = (uint32_t)request->interval_ms;
    }
    const char *name = request->name;
    const char *host = request->host;
    int port = request->port;

    if (name != NULL && name[0] != '\0') {
        airplay_target_t target;
        if (!discovery_find(name, &target)) {
            airplay_target_t found[DISCOVERY_MAX_TARGETS];
            int count = discovery_snapshot(found, DISCOVERY_MAX_TARGETS);
            size_t used = (size_t)snprintf(s_reply, sizeof(s_reply),
                                           "no receiver matches \"%s\"", name);
            if (count > 0) {
                for (int i = 0; i < count; i++) {
                    int written = snprintf(s_reply + used, sizeof(s_reply) - used, "%s%s",
                                           i == 0 ? "; saw " : ", ", found[i].name);
                    if (written <= 0 || (size_t)written >= sizeof(s_reply) - used) {
                        break;
                    }
                    used += (size_t)written;
                }
                snprintf(s_reply + used, sizeof(s_reply) - used, "\n");
            } else {
                snprintf(s_reply + used, sizeof(s_reply) - used,
                         "; GET /targets lists the receivers on the LAN\n");
            }
            return s_reply;
        }
        strlcpy(s_target_host, target.host, sizeof(s_target_host));
        strlcpy(s_target_name, target.name, sizeof(s_target_name));
        s_config.port = target.port;
        /* The advertised model/version decides the protocol generation: an
         * AppleTV3,2-style box only speaks legacy AirTunes. */
        s_config.legacy = target.legacy;
        ESP_LOGI(TAG, "\"%s\" resolved to %s:%u (%s)", name, s_target_host,
                 (unsigned)s_config.port, target.legacy ? "legacy AirTunes" : "HAP");
    } else if (host != NULL && host[0] != '\0') {
        strlcpy(s_target_host, host, sizeof(s_target_host));
        strlcpy(s_target_name, host, sizeof(s_target_name));
        if (port > 0) {
            s_config.port = (uint16_t)port;
        }
    }

    s_session_stop = false;
    xTaskNotifyGive(s_session_task);
    return "starting\n";
}

static const char *hook_targets(char *buf, size_t len)
{
    airplay_target_t targets[DISCOVERY_MAX_TARGETS];
    int count = discovery_snapshot(targets, DISCOVERY_MAX_TARGETS);
    if (count == 0) {
        snprintf(buf, len, "no AirPlay receiver on the LAN\n");
        return buf;
    }
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        int written = snprintf(buf + used, len - used, "%s - %s:%u%s\n", targets[i].name,
                               targets[i].host, (unsigned)targets[i].port,
                               targets[i].legacy ? " [legacy]" : "");
        if (written <= 0 || (size_t)written >= len - used) {
            break;
        }
        used += (size_t)written;
    }
    return buf;
}

static const char *hook_forget(const char *name)
{
    static char reply[96];
    if (discovery_forget(name)) {
        ESP_LOGI(TAG, "receiver \"%s\" forgotten", name);
        snprintf(reply, sizeof(reply), "forgotten %s\n", name);
    } else {
        snprintf(reply, sizeof(reply), "no receiver named %s\n", name);
    }
    return reply;
}

/* Sets the live source's on-screen text; an empty msg hands the canvas back
 * to the test pattern. Runs on the HTTP task, and media_text_set() is built
 * for exactly that: it flips buffers instead of taking a lock. */
static const char *hook_text(const char *msg)
{
    static char reply[MEDIA_TEXT_MAX_BYTES + 16];

    if (msg == NULL) {
        char current[MEDIA_TEXT_MAX_BYTES];
        media_text_get(current, sizeof(current));
        if (current[0] == '\0') {
            return "text: (test pattern)\n";
        }
        snprintf(reply, sizeof(reply), "text: %s\n", current);
        return reply;
    }
    if (!media_text_set(msg)) {
        return "text does not fit (max 127 bytes of UTF-8)\n";
    }
    ESP_LOGI(TAG, "on-screen text %s: \"%s\"", msg[0] != '\0' ? "set" : "cleared", msg);
    return msg[0] != '\0' ? "text set\n" : "text cleared; the test pattern is back\n";
}

static const char *hook_stop(void)
{
    if (!airplay_is_running()) {
        return "not mirroring\n";
    }
    s_session_stop = true;
    airplay_request_stop();
    return "stopping\n";
}

static const char *hook_status(void)
{
    if (airplay_is_running()) {
        return "mirroring\n";
    }
    /* The audio counters only exist once a session has run; they answer the one
     * question the ears cannot: did the sound arrive on time, or did it starve? */
    static char line[160];
    airplay_audio_stats_t audio = { 0 };
    airplay_last_audio_stats(&audio);
    if (audio.packets != 0) {
        snprintf(line, sizeof(line), "%saudio: %llu packet(s), %llu without a frame, "
                                     "%llu late (max %llu ms)\n",
                 s_last_result, (unsigned long long)audio.packets,
                 (unsigned long long)audio.gaps, (unsigned long long)audio.late,
                 (unsigned long long)audio.late_max_ms);
        return line;
    }
    return s_last_result;
}

static void log_banner(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    uint8_t mac[6] = {0};
    /* The base MAC exists on every chip; the Wi-Fi STA MAC only exists where
     * the radio is local (on the P4 the radio is the ESP32-C6 behind
     * ESP-Hosted). */
    esp_read_mac(mac, ESP_MAC_BASE);

    ESP_LOGI(TAG, "==== AirPlay sender on %s ====", CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "cores=%d revision=%d.%d flash=%" PRIu32 " MB",
             chip.cores, chip.revision / 100, chip.revision % 100, flash_size / (1024 * 1024));
    ESP_LOGI(TAG, "mac=" MACSTR " free heap=%" PRIu32 " B",
             MAC2STR(mac), esp_get_free_heap_size());
}

/* Bench hook: some networks cannot reach the device, so a task polls the host
 * for the code and publishes it like a web-page submission. Running it in its
 * own task keeps a stuck socket away from the pairing flow. */
static void pin_fetch_task(void *argument)
{
    (void)argument;
    for (;;) {
        char code[5];
        if (pin_fetch_from_host(APP_PIN_FETCH_HOST, APP_PIN_FETCH_PORT, code)) {
            pin_server_submit(code);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* Reads digits from the console in its own task: a console read can block, and
 * the pairing flow must never wait on it. */
static void console_pin_task(void *argument)
{
    (void)argument;
    char code[5];
    int typed = 0;

    int previous_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (previous_flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, previous_flags | O_NONBLOCK);
    }
    for (;;) {
        char key;
        if (read(STDIN_FILENO, &key, 1) == 1) {
            if (key >= '0' && key <= '9') {
                code[typed++] = key;
                if (typed == 4) {
                    code[4] = '\0';
                    pin_server_submit(code);
                    typed = 0;
                }
            } else {
                typed = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* Waits for the code the receiver is showing. It arrives either from the small
 * HTTP page (the USB console cannot accept input) or from the console, and the
 * wait is a poll so the idle task keeps running. */
static bool prompt_pin(char pin[5], void *user_data)
{
    (void)user_data;
    if (APP_PAIRING_PIN[0] != '\0') {
        snprintf(pin, 5, "%.4s", APP_PAIRING_PIN);
        return true;
    }

    /* Anything already queued belongs to a previous request and is expired. */
    pin_server_flush();
    ESP_LOGW(TAG, "waiting for the pairing code (%s:%d/pin, web page, or console)",
             APP_PIN_FETCH_HOST, APP_PIN_FETCH_PORT);

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(180000);
    while (xTaskGetTickCount() < deadline) {
        if (s_session_stop) {
            ESP_LOGW(TAG, "stop requested; giving up on the pairing code");
            return false;
        }
        if (pin_server_wait(pin, 5000)) {
            ESP_LOGI(TAG, "pairing code accepted");
            return true;
        }
    }

    ESP_LOGE(TAG, "no code was entered; set APP_PAIRING_PIN in app_config_local.h to skip the prompt");
    return false;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    log_banner();

    /* The live encoder takes about a megabyte, so claim it before anything
     * else has a chance to fragment the heap. When it fails, mirroring falls
     * back to the flash-resident still images. */
    bool live_video = false;
#if APP_USE_LIVE_VIDEO
    live_video = media_live_init();
    if (!live_video) {
        ESP_LOGE(TAG, "live video unavailable; mirroring will use the still images");
    }
#endif
#if APP_USE_NETWORK_VIDEO
    if (!media_net_init(APP_VIDEO_INPUT_PORT)) {
        ESP_LOGE(TAG, "capture input unavailable; a macOS capture client cannot connect");
    }
#endif
#if APP_RUN_SELFTEST
    if (fp_selftest() != 0) {
        ESP_LOGE(TAG, "FairPlay self-test FAILED: the handshake cannot work, fix the port first");
    }
#else
    ESP_LOGW(TAG, "self-test disabled (APP_RUN_SELFTEST=0)");
#endif

    hap_identity_t *identity = &s_identity;
    if (app_identity_load(identity) != ESP_OK) {
        ESP_LOGE(TAG, "no controller identity; pairing will not survive a reboot");
    }

    wifi_net_config_t network = {
        .ssid = APP_WIFI_SSID,
        .password = APP_WIFI_PASSWORD,
        .ap_ssid = APP_FALLBACK_AP_SSID,
        .ap_password = APP_FALLBACK_AP_PASSWORD,
    };
    err = wifi_net_start(&network);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network unavailable: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "fill in APP_WIFI_SSID/APP_WIFI_PASSWORD in app_config_local.h and reboot");
        return;
    }

    char *local_ip = s_local_ip;
    wifi_net_ip(local_ip, sizeof(s_local_ip));
    ESP_LOGI(TAG, "local address %s", local_ip);
#if APP_USE_NETWORK_VIDEO
    /* Sockets need lwIP, which only exists once the network is up. */
    if (!media_net_start()) {
        ESP_LOGE(TAG, "cannot start the capture listener");
    }
#endif
    if (discovery_start() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS discovery unavailable; /targets will not work");
    }
    if (pin_server_start() != ESP_OK) {
        ESP_LOGW(TAG, "pairing web page unavailable; PIN can still arrive over the bench hook");
    }
    if (xTaskCreate(console_pin_task, "pin_console", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "cannot start the console reader");
    }
    if (APP_PIN_FETCH_PORT != 0) {
        if (xTaskCreate(pin_fetch_task, "pin_fetch", 6144, NULL, 3, NULL) != pdPASS) {
            ESP_LOGW(TAG, "cannot start the pairing-code fetch task");
        }
    }
    strlcpy(s_target_host, APP_AIRPLAY_TARGET_HOST, sizeof(s_target_host));
    strlcpy(s_target_name, APP_AIRPLAY_TARGET_NAME, sizeof(s_target_name));
    airplay_config_t config = {
        .host = s_target_host,
        .port = APP_AIRPLAY_TARGET_PORT,
        .device_name = APP_DEVICE_NAME,
        .local_address = local_ip,
        .pin = APP_PAIRING_PIN,
        .force_pin = APP_FORCE_PIN_PAIRING != 0,
        .request_pin = prompt_pin,
        .frames = media_still_frames(),
        .frame_interval_ms = APP_FRAME_INTERVAL_MS,
        .identity = identity,
        .legacy = APP_AIRPLAY_LEGACY != 0,
        .legacy_data_mode = APP_LEGACY_DATA_MODE,
        .legacy_stream_port = APP_LEGACY_STREAM_PORT,
        .legacy_key_mode = (uint8_t)(APP_LEGACY_KEY_MIX ? 1 : 0),
        .legacy_iv_source = APP_LEGACY_IV_SOURCE,
        .legacy_inband_sps = APP_LEGACY_INBAND_SPS != 0,
        .legacy_pairing = APP_LEGACY_PAIRING != 0,
    };
    s_config = config;
    ESP_LOGI(TAG, "default receiver %s:%d, GET /targets lists what else is on the LAN",
             s_target_host, APP_AIRPLAY_TARGET_PORT);
    select_video_source();

    static const pin_server_session_hooks_t hooks = {
        .start = hook_start,
        .stop = hook_stop,
        .status = hook_status,
        .targets = hook_targets,
        .forget = hook_forget,
        .text = hook_text,
    };
    pin_server_set_session_hooks(&hooks);
    /* The session task runs the encoder itself, and openh264's slice and
     * rate-control paths nest deeply; 8 KB is not enough for that on RISC-V. */
    if (xTaskCreate(session_task, "session", 16384, NULL, 4, &s_session_task) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the session task; mirroring is unavailable");
        return;
    }
    ESP_LOGI(TAG, "idle; POST/GET http://%s:%d/start to begin mirroring (optionally with "
             "?name=<receiver>)", local_ip, PIN_SERVER_PORT);
}
