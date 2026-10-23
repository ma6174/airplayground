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

/* Set to 1 for receivers that ask for an on-screen code (Apple TV, macOS
 * AirPlay receiver): go straight to the code flow, because a failed
 * empty-PIN attempt makes the receiver throttle the next pairing. */
#ifndef APP_FORCE_PIN_PAIRING
#define APP_FORCE_PIN_PAIRING 0
#endif

/* 4-digit code for receivers that demand one. Leave empty to be prompted on the
 * console when the receiver asks (Apple TV shows the code on screen only when
 * the session needs it). */
#ifndef APP_PAIRING_PIN
#define APP_PAIRING_PIN ""
#endif

/* Bench helper: when non-zero, the device polls http://<receiver host>:<port>/pin
 * for the pairing code. Useful when the network does not allow the browser or
 * curl to reach the device itself. Zero disables it. */
#ifndef APP_PIN_FETCH_PORT
#define APP_PIN_FETCH_PORT 0
#endif

/* Host that serves the code (defaults to the receiver). */
#ifndef APP_PIN_FETCH_HOST
#define APP_PIN_FETCH_HOST APP_AIRPLAY_TARGET_HOST
#endif

/* Talk the legacy AirTunes protocol instead of HAP: for third-party receivers
 * that advertise AppleTV3,2 / AirTunes 220.x (many projectors). */
#ifndef APP_AIRPLAY_LEGACY
#define APP_AIRPLAY_LEGACY 0
#endif

/* How the legacy mirror stream reaches a receiver that hands out no dataPort:
 *   1 = RTSP interleaved (`$`+channel+length) on the control connection
 *   2 = raw records ([128-byte header][payload]) on the control connection
 *   3 = interleaved on a second connection to the RTSP port
 *   4 = raw records on a second connection to the RTSP port
 * Receivers that do answer SETUP with a dataPort ignore this. */
#ifndef APP_LEGACY_DATA_MODE
#define APP_LEGACY_DATA_MODE 1
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
