/*
 * Tiny HTTP endpoint for handing the pairing code to the device.
 *
 * The console cannot be used for this: on the USB-Serial/JTAG port the host can
 * read the console but the bytes it writes never reach the device's console
 * input. A form on the LAN works from any browser or curl and needs no cable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define PIN_SERVER_PORT 8080

/* Query overrides for one session: -1 (or 0 for port) keeps the configured
 * value. Handy for probing a receiver without reflashing. */
typedef struct {
    const char *name;
    const char *host;
    int port;
    int data_mode;
    int key_mix;
    int iv_source;
    int interval_ms;
    int sps;
    int pair;
    int audio;
    int amix;
    int stream_port;
} pin_start_request_t;

esp_err_t pin_server_start(void);

/* Handlers for the session-control endpoints. Each returns a short one-line
 * answer; NULL hooks make the endpoints report that session control or
 * discovery is unavailable. */
typedef struct {
    /* name and host come from the /start query string (?name= or ?host=, plus
     * an optional ?port=); NULL or empty means the configured default
     * receiver. */
    const char *(*start)(const pin_start_request_t *request);
    const char *(*stop)(void);
    const char *(*status)(void);
    /* Writes the discovered receiver list into buf; returns buf, or NULL when
     * the browse failed. The browse blocks for a couple of seconds. */
    const char *(*targets)(char *buf, size_t len);
    /* Drops the receiver with this name from the list. */
    const char *(*forget)(const char *name);
} pin_server_session_hooks_t;

void pin_server_set_session_hooks(const pin_server_session_hooks_t *hooks);

/* Returns true and fills pin (4 digits + NUL) once a code has been submitted. */
bool pin_server_take(char pin[5]);

/* Drops codes that were produced for an earlier prompt: a code is only valid
 * for the pin-start that created it. */
void pin_server_flush(void);

/* Same, but blocks up to timeout_ms waiting for one. */
bool pin_server_wait(char pin[5], uint32_t timeout_ms);

/* Publishes a code from another input channel (the bench fetch task). */
void pin_server_submit(const char pin[5]);
