/*
 * Network bring-up: join the configured Wi-Fi network, or - when no SSID is
 * configured - publish our own access point so a host can join and run a
 * receiver on 192.168.4.2.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct {
    const char *ssid;        /* empty starts the fallback access point */
    const char *password;
    const char *ap_ssid;
    const char *ap_password;
} wifi_net_config_t;

/* Brings the radio up and waits (up to ~30 s) for an address. */
esp_err_t wifi_net_start(const wifi_net_config_t *config);

/* Writes the local IPv4 address as dotted quad. */
esp_err_t wifi_net_ip(char *out, size_t out_len);

bool wifi_net_is_access_point(void);
