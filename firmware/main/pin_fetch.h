/*
 * Bench helper: fetch the pairing code from a host that the device can reach.
 *
 * Some networks block traffic towards the device (a guest network with client
 * isolation, or a host whose 100.64/10 route belongs to a VPN). The device can
 * still open outbound connections, so it polls a plain HTTP file instead.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Returns true and fills pin (4 digits + NUL) when host:port/pin answers with a
 * code. Never blocks longer than the connect/read timeouts. */
bool pin_fetch_from_host(const char *host, uint16_t port, char pin[5]);
