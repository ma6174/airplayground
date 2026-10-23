#include "wifi_net.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi";

#define CONNECTED_BIT BIT0
#define FAILED_BIT BIT1
#define STA_ATTEMPTS 3

static EventGroupHandle_t s_events;
static bool s_access_point;
static esp_netif_t *s_netif;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            static int attempts;
            attempts++;
            if (attempts >= STA_ATTEMPTS) {
                ESP_LOGE(TAG, "could not join %s after %d attempts", (const char *)arg, attempts);
                xEventGroupSetBits(s_events, FAILED_BIT);
            } else {
                ESP_LOGW(TAG, "disconnected, retrying (%d/%d)", attempts, STA_ATTEMPTS);
                esp_wifi_connect();
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            const wifi_event_ap_staconnected_t *event = data;
            ESP_LOGI(TAG, "host joined the access point: " MACSTR, MAC2STR(event->mac));
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "got address " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

static esp_err_t start_station(const wifi_net_config_t *config)
{
    s_access_point = false;
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t station = {0};
    strlcpy((char *)station.sta.ssid, config->ssid, sizeof(station.sta.ssid));
    strlcpy((char *)station.sta.password, config->password, sizeof(station.sta.password));
    /* Open networks exist; let the driver decide whether to skip auth. */
    station.sta.threshold.authmode =
        config->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    station.sta.pmf_cfg.capable = true;

    ESP_LOGI(TAG, "joining \"%s\"", config->ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Modem sleep (the default) drops multicast between DTIMs, and mDNS
     * announcements are multicast: receivers that only announce on boot would
     * never be seen. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT | FAILED_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(30000));
    if (bits & CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & FAILED_BIT) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t start_access_point(const wifi_net_config_t *config)
{
    s_access_point = true;
    s_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t access_point = {0};
    strlcpy((char *)access_point.ap.ssid, config->ap_ssid, sizeof(access_point.ap.ssid));
    strlcpy((char *)access_point.ap.password, config->ap_password,
            sizeof(access_point.ap.password));
    access_point.ap.ssid_len = strlen(config->ap_ssid);
    access_point.ap.channel = 6;
    access_point.ap.max_connection = 2;
    access_point.ap.authmode = config->ap_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_LOGI(TAG, "starting access point \"%s\" on channel %d", config->ap_ssid,
             access_point.ap.channel);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &access_point));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip = {0};
    ESP_ERROR_CHECK(esp_netif_get_ip_info(s_netif, &ip));
    ESP_LOGI(TAG, "access point ready: join \"%s\", device is " IPSTR, config->ap_ssid,
             IP2STR(&ip.ip));
    return ESP_OK;
}

esp_err_t wifi_net_start(const wifi_net_config_t *config)
{
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, (void *)config->ssid, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi_event, (void *)config->ssid, NULL));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (config->ssid[0] == '\0') {
        return start_access_point(config);
    }
    return start_station(config);
}

esp_err_t wifi_net_ip(char *out, size_t out_len)
{
    esp_netif_ip_info_t ip = {0};
    if (s_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_netif_get_ip_info(s_netif, &ip);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(out, out_len, IPSTR, IP2STR(&ip.ip));
    return ESP_OK;
}

bool wifi_net_is_access_point(void)
{
    return s_access_point;
}
