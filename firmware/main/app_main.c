#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
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
#include "wifi_net.h"

static const char *TAG = "app";

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

/* Reads the code the receiver is showing. The UART console has no select()
 * support, so the fd is switched to non-blocking and polled instead of being
 * waited on: a spin here would starve the idle task and trip the watchdog. */
static bool prompt_pin(char pin[5], void *user_data)
{
    (void)user_data;
    if (APP_PAIRING_PIN[0] != '\0') {
        snprintf(pin, 5, "%.4s", APP_PAIRING_PIN);
        return true;
    }

    printf("\n*** The receiver demands a pairing code.\n"
           "*** Enter the 4 digits shown on its screen: ");
    fflush(stdout);

    int previous_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (previous_flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, previous_flags | O_NONBLOCK);
    }

    int typed = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(60000);
    while (typed < 4 && xTaskGetTickCount() < deadline) {
        int key = fgetc(stdin);
        if (key == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (key < '0' || key > '9') {
            continue;
        }
        pin[typed++] = (char)key;
        fputc(key, stdout);
        fflush(stdout);
    }
    pin[typed] = '\0';
    printf("\n");

    if (previous_flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, previous_flags);
    }
    if (typed != 4) {
        ESP_LOGE(TAG, "no code entered; set APP_PAIRING_PIN in app_config_local.h to skip the prompt");
        return false;
    }
    return true;
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

    hap_identity_t identity;
    if (app_identity_load(&identity) != ESP_OK) {
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

    char local_ip[16];
    wifi_net_ip(local_ip, sizeof(local_ip));
    ESP_LOGI(TAG, "local address %s, receiver %s:%d", local_ip, APP_AIRPLAY_TARGET_HOST,
             APP_AIRPLAY_TARGET_PORT);

    airplay_config_t config = {
        .host = APP_AIRPLAY_TARGET_HOST,
        .port = APP_AIRPLAY_TARGET_PORT,
        .device_name = APP_DEVICE_NAME,
        .local_address = local_ip,
        .pin = APP_PAIRING_PIN,
        .request_pin = prompt_pin,
        .frames = media_still_frames(),
        .frame_interval_ms = APP_FRAME_INTERVAL_MS,
        .identity = &identity,
    };

    for (;;) {
        err = airplay_mirror(&config);
        ESP_LOGW(TAG, "session finished (%s); retrying in 10 s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
