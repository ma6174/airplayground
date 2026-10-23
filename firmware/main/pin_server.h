/*
 * Tiny HTTP endpoint for handing the pairing code to the device.
 *
 * The console cannot be used for this: on the USB-Serial/JTAG port the host can
 * read the console but the bytes it writes never reach the device's console
 * input. A form on the LAN works from any browser or curl and needs no cable.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define PIN_SERVER_PORT 8080

esp_err_t pin_server_start(void);

/* Handlers for the session-control endpoints (/start, /stop, /status). Each
 * returns a short one-line answer; NULL hooks make the endpoints report that
 * session control is unavailable. */
typedef struct {
    const char *(*start)(void);
    const char *(*stop)(void);
    const char *(*status)(void);
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
