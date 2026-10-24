/* Network-fed live video source: the macOS capture-cli encodes the screen with
 * VideoToolbox and streams the access units here, so the chip does not spend
 * its CPU on H.264 encoding and the mirrored picture is the real screen.
 *
 * The client speaks a small length-prefixed protocol: one configure packet
 * (geometry plus SPS/PPS) followed by access units, each already in AVCC
 * framing and tagged with its IDR flag. The access units go straight into the
 * AirPlay data channel.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airplay.h"

/* Starts the listener. Returns false when the task or its semaphore cannot be
 * created; the port is taken from APP_VIDEO_INPUT_PORT. */
bool media_net_init(uint16_t port);

/* Creates the listener task. Call once the network is up: sockets must not be
 * touched before lwIP is initialised. */
bool media_net_start(void);

/* True once a capture client has delivered its configure packet. The session
 * falls back to its other content sources until then. */
bool media_net_ready(void);

/* The source for airplay_config_t.live_source, or NULL when no capture client
 * is configured. Geometry tracks the client's configure packet, so call it when
 * the session starts rather than holding on to the pointer across sessions. */
const airplay_live_source_t *media_net_source(void);
