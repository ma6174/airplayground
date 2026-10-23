/*
 * Receiver-initiated event channel.
 *
 * The sender dials the eventPort from a SETUP reply and then only reads: the
 * receiver sends RTSP requests on this connection and expects a 200
 * acknowledgement for each one. Encryption mirrors the control channel with the
 * key directions reversed, because the write direction is the receiver's.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hap.h"

struct ap_session;

typedef struct {
    int fd;
    hap_channel_t channel;
    bool active;
} ap_event_channel_t;

/* Dials the receiver's event port and starts the reader task. Returns 0 when
 * the channel is up; the session continues without it on failure. */
int ap_event_start(struct ap_session *session, uint16_t port);
void ap_event_stop(struct ap_session *session);
