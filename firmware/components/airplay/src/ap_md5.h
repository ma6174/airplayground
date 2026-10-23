/*
 * MD5, only because RFC 2069 Digest authentication names it. Nothing else in
 * the AirPlay path uses it, and the algorithm is fixed by the RFC, so a compact
 * implementation keeps the host build free of an mbedtls dependency while
 * staying byte-identical to the device.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

void ap_md5(const void *data, size_t len, uint8_t digest[16]);

/* Lower-case hex of ap_md5(). out must hold 33 bytes. */
void ap_md5_hex(const void *data, size_t len, char out[33]);
