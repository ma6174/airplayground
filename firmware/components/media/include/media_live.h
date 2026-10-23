/*
 * The live video source: an OpenH264 (esp_h264 software) encoder fed by the
 * built-in test pattern, wrapped into the access-unit format the AirPlay data
 * channel sends.
 *
 * Memory is the constraint: the codec wants roughly a megabyte for its state
 * on top of the picture buffers. Large buffers prefer PSRAM and fall back to
 * internal RAM; when neither fits, media_live_init() reports failure and the
 * app serves the flash-resident still images instead.
 */
#pragma once

#include <stdbool.h>

#include "airplay.h"

/* Creates the encoder and its buffers. Safe to call once at boot; returns
 * false when the chip cannot spare the memory. */
bool media_live_init(void);

/* True when media_live_init() succeeded. */
bool media_live_ready(void);

/* The source for airplay_config_t.live_source. Only valid after a successful
 * media_live_init(). */
const airplay_live_source_t *media_live_source(void);
