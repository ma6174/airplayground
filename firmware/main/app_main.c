#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "airplay.h"
#include "app_config.h"
#include "app_identity.h"
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
#include "nvs_flash.h"
#include "pin_fetch.h"
#include "pin_server.h"
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
        ESP_LOGI(TAG, "starting a mirroring session on request");
        esp_err_t err = airplay_mirror(&s_config);
        snprintf(s_last_result, sizeof(s_last_result),
                 err == ESP_OK ? "last session: ok" : "last session: %s",
                 err == ESP_OK ? "" : esp_err_to_name(err));
        /* The session ends on failure, on /stop, or when the receiver goes
         * away. Nothing restarts it automatically: /start does that. */
        s_session_stop = false;
        ESP_LOGI(TAG, "%s", s_last_result);
    }
}

static const char *hook_start(void)
{
    if (airplay_is_running()) {
        return "already mirroring\n";
    }
    s_session_stop = false;
    xTaskNotifyGive(s_session_task);
    return "starting\n";
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
    return airplay_is_running() ? "mirroring\n" : s_last_result;
}

static void log_banner(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    ESP_LOGI(TAG, "==== AirPlay sender on ESP32-S3 ====");
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
    ESP_LOGI(TAG, "local address %s, receiver %s:%d", local_ip, APP_AIRPLAY_TARGET_HOST,
             APP_AIRPLAY_TARGET_PORT);

    airplay_config_t config = {
        .host = APP_AIRPLAY_TARGET_HOST,
        .port = APP_AIRPLAY_TARGET_PORT,
        .device_name = APP_DEVICE_NAME,
        .local_address = local_ip,
        .pin = APP_PAIRING_PIN,
        .force_pin = APP_FORCE_PIN_PAIRING != 0,
        .request_pin = prompt_pin,
        .frames = media_still_frames(),
        .frame_interval_ms = APP_FRAME_INTERVAL_MS,
        .identity = identity,
    };
    s_config = config;

    static const pin_server_session_hooks_t hooks = {
        .start = hook_start,
        .stop = hook_stop,
        .status = hook_status,
    };
    pin_server_set_session_hooks(&hooks);
    if (xTaskCreate(session_task, "session", 8192, NULL, 4, &s_session_task) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the session task; mirroring is unavailable");
        return;
    }
    ESP_LOGI(TAG, "idle; POST/GET http://%s:%d/start to begin mirroring", local_ip,
             PIN_SERVER_PORT);
}
