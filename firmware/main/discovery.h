/*
 * LAN discovery for AirPlay mirroring receivers, so sessions can be started
 * by receiver name instead of a hard-coded address.
 *
 * Two kinds of responders are out there. Well-behaved ones (macOS receiver,
 * Apple TV) answer mDNS queries. Cheap AirTunes-style boxes (model=AppleTV3,2,
 * srcvers 220.x) only announce on boot and ignore every query, so an active
 * browse never sees them. The module therefore runs both paths: one-shot
 * queries plus a persistent passive browse that keeps its own cache of what
 * the LAN announces, and one entry per receiver address with a legacy flag.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* mDNS instance names are limited to 63 bytes + NUL. */
#define DISCOVERY_NAME_LEN 64
/* One bench network already runs several receivers per machine (each UxPlay
 * instance is its own device), so the list has to hold more than a handful. */
#define DISCOVERY_MAX_TARGETS 16

typedef struct {
    char name[DISCOVERY_NAME_LEN]; /* the name shown in the receiver list */
    char host[16];                 /* dotted quad */
    uint16_t port;
    /* AirTunes-style receiver (model=AppleTV3,2 or srcvers 220.x): needs the
     * legacy session path instead of HAP pairing. */
    bool legacy;
} airplay_target_t;

/* Brings up the mDNS client and starts the passive browse. Call once the
 * network interface is up. */
esp_err_t discovery_start(void);

/* Runs the active browse once and refreshes the list. Normally the background
 * refresh keeps the list warm and callers only need discovery_snapshot(); this
 * is for the rare blocking call (e.g. the list is empty and a session was
 * requested by name). Receivers that answer queries are re-learned every time
 * and stale ones age out after ten minutes; legacy receivers are kept for good
 * and survive reboots (stored in NVS) since they can only be learned from
 * their rare announcements. Returns the number of receivers known afterwards,
 * or -1 when the query itself failed. Blocks for about three seconds. */
int discovery_browse(void);

/* The current receiver list, refreshed in the background every 30 s. Copies
 * out so callers never block. */
int discovery_snapshot(airplay_target_t *out, int max);

/* Finds a receiver by name: exact (case-insensitive) first, then a substring
 * match when it points at exactly one receiver. Browses first when the list
 * is empty. */
bool discovery_find(const char *name, airplay_target_t *out);

/* Drops the receiver with this exact (case-insensitive) name from the list,
 * including its persisted record for legacy receivers. Returns false when no
 * receiver carries that name. */
bool discovery_forget(const char *name);

/* Records a receiver this device has just mirrored to. Receivers behind a
 * network that swallows their multicast (or that only announce once per power
 * cycle) never answer a browse, so a working session is the only evidence the
 * list will ever get. */
void discovery_remember(const char *name, const char *host, uint16_t port, bool legacy);
