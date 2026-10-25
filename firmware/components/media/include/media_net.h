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

/* Pull mode: the same protocol, but this side opens the connection instead of
 * waiting for the client. A server that cannot be dialled from outside - a
 * laptop behind NAT, a machine on another network - can then still feed the
 * screen, and the device re-establishes the stream on its own after a reboot
 * rather than waiting for the server to notice it went away.
 *
 * The target is remembered in NVS. An empty host clears it. */
bool media_net_pull(const char *host, uint16_t port);

/* Forgets the target, breaks a connection that is already pulling, and goes
 * back to waiting for a client. */
void media_net_pull_stop(void);

/* "host:port" while a target is set, "" otherwise. */
const char *media_net_pull_target(void);

/* The source for airplay_config_t.live_source, or NULL when no capture client
 * is configured. Geometry tracks the client's configure packet, so call it when
 * the session starts rather than holding on to the pointer across sessions. */
const airplay_live_source_t *media_net_source(void);

/* Audio arrives on its own connection, one port above the video one: a keyframe
 * on a shared connection would hold the audio writes back long enough to be
 * heard. */
#define MEDIA_NET_AUDIO_PORT_OFFSET 1

/* Pops one encoded audio frame the capture client sent, or returns NULL when
 * none is queued. The bytes are handed over in a buffer owned here, valid until
 * the next call, so the caller's copy cannot be overwritten by the client's
 * next frame.
 *
 * The ring is emptied when a session starts, which throws away the lead the
 * client had built up. One byte is sent back on the audio connection at that
 * moment: a client that reads it knows to rebuild the lead instead of letting
 * the receiver starve on an empty ring. Clients that ignore it see the byte
 * pile up in their receive buffer and nothing else. */
const uint8_t *media_net_audio(size_t *len);

/* How many audio frames the client actually delivered since the stream started.
 * Comparing this with the packets we managed to send says whether the sound
 * starved because the client held back or because we did. */
uint32_t media_net_audio_received(void);
