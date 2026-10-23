/*
 * The AirPlay control connection: one TCP stream carrying RTSP/1.0 requests,
 * wrapped in HAP ChaCha20-Poly1305 framing once pair-verify has completed.
 *
 * Everything here is sequential - the receiver answers in order and the session
 * sends one request at a time - so the client owns the CSeq counter, the fixed
 * response buffer and the cached Digest challenge.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hap.h"

#define AP_HEADER_MAX 1024
#define AP_BODY_MAX 4096
#define AP_REQUEST_MAX 2048

#define AP_CONTROL_CONNECT_TIMEOUT_MS 10000

typedef struct {
    const char *name;
    const char *value;
} ap_header_t;

typedef struct {
    int status;
    size_t body_len;
    bool has_challenge;
    char realm[64];
    char nonce[128];
} ap_response_t;

typedef struct {
    int fd;
    hap_channel_t channel;
    uint32_t cseq;
    bool encrypted;
    const char *password; /* Digest password, or NULL when authentication is unavailable */
    bool has_challenge;
    char realm[64];
    char nonce[128];
    /* PTP clock anchor: Apple receivers stamp SETUP replies with the time they
     * received the request and how long they spent on it, both in decimal
     * milliseconds. Absent on third-party receivers, which is why the session
     * keeps a boot-clock fallback. */
    bool has_clock_headers;
    uint64_t received_timestamp_ms;
    uint64_t processing_time_ms;
    /* Headers every request carries (DACP-ID / Active-Remote). Apple's senders
     * always send them and some receivers key their session on them. */
    char extra_headers[2][64];

    /* Fixed request and reply buffers: control bodies are small plists, and a
     * receiver must not be able to make the sender allocate. */
    uint8_t request[AP_REQUEST_MAX];
    uint8_t body[AP_BODY_MAX];
    size_t body_len;
} ap_client_t;

int ap_client_connect(ap_client_t *client, const char *host, uint16_t port, uint32_t timeout_ms);

/* Adopts an already-connected socket. Used by the host tests to drive the
 * parser over a socketpair. */
void ap_client_attach_fd(ap_client_t *client, int fd);

/* Sets a header that each request will carry (slot 0 or 1, format "Name: value"). */
void ap_client_set_extra_header(ap_client_t *client, int slot, const char *line);

void ap_client_close(ap_client_t *client);
void ap_client_set_password(ap_client_t *client, const char *password);
void ap_client_enable_encryption(ap_client_t *client, const uint8_t write_key[HAP_KEY_LEN],
                                 const uint8_t read_key[HAP_KEY_LEN]);

/* Builds one request into out and advances the connection's CSeq. Returns the
 * byte count, or 0 when out is too small. */
size_t ap_client_build(ap_client_t *client, const char *method, const char *uri, const char *content_type,
                       const void *body, size_t body_len, const ap_header_t *headers,
                       const char *authorization, void *out, size_t out_cap);

/* Sends one request and reads its response, retrying once after a 401 Digest
 * challenge. Returns 0 for a 2xx status, -1 otherwise (already logged). */
int ap_client_request(ap_client_t *client, const char *method, const char *uri, const char *content_type,
                      const void *body, size_t body_len, const ap_header_t *headers,
                      ap_response_t *response);

const void *ap_client_body(const ap_client_t *client, size_t *len);
