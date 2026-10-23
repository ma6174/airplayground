/*
 * Build-time configuration for the AirPlay sender firmware.
 *
 * Everything that is machine specific (Wi-Fi credentials, receiver address)
 * lives in app_config_local.h, which is git-ignored. Copy
 * app_config_local.h.example to app_config_local.h and fill it in.
 */
#pragma once

#if defined(__has_include)
#if __has_include("app_config_local.h")
#include "app_config_local.h"
#endif
#endif

/* Empty SSID means "start the fallback access point instead of joining a
 * network". That mode is useful for bench testing: the host joins the
 * ESP32's own AP and runs a receiver on 192.168.4.2. */
#ifndef APP_WIFI_SSID
#define APP_WIFI_SSID ""
#endif

#ifndef APP_WIFI_PASSWORD
#define APP_WIFI_PASSWORD ""
#endif

/* AirPlay receiver to mirror into (Apple TV or a test receiver). */
#ifndef APP_AIRPLAY_TARGET_HOST
#define APP_AIRPLAY_TARGET_HOST "192.168.1.10"
#endif

#ifndef APP_AIRPLAY_TARGET_PORT
#define APP_AIRPLAY_TARGET_PORT 7000
#endif

/* Name the receiver shows in its mirroring UI. */
#ifndef APP_DEVICE_NAME
#define APP_DEVICE_NAME "ESP32-S3 AirPlay"
#endif

/* 4-digit code for receivers that demand one. Leave empty to be prompted on the
 * console when the receiver asks (Apple TV shows the code on screen only when
 * the session needs it). */
#ifndef APP_PAIRING_PIN
#define APP_PAIRING_PIN ""
#endif

/* Fallback access point, used when APP_WIFI_SSID is empty. */
#ifndef APP_FALLBACK_AP_SSID
#define APP_FALLBACK_AP_SSID "airplay-esp32"
#endif

#ifndef APP_FALLBACK_AP_PASSWORD
#define APP_FALLBACK_AP_PASSWORD "airplay1234"
#endif

/* How long each still image stays on screen before the next one is sent. */
#ifndef APP_FRAME_INTERVAL_MS
#define APP_FRAME_INTERVAL_MS 10000
#endif

/* Keep the FairPlay self-test at boot: it re-derives every published test
 * vector on the device and is the only ground truth we have before a receiver
 * is reachable. */
#ifndef APP_RUN_SELFTEST
#define APP_RUN_SELFTEST 1
#endif
