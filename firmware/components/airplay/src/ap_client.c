#include "ap_client.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ap_md5.h"
#include "ap_platform.h"

static const char *TAG = "airplay";

/* Fixed by the protocol; senders do not negotiate a version. */
#define AP_USER_AGENT "AirPlay/935.7.1"
#define AP_DIGEST_USERNAME "AirPlay"

typedef struct {
    uint8_t *out;
    size_t cap;
    size_t len;
    bool ok;
} writer_t;

/* Deliberately not select()-based: an encrypted read may be served entirely
 * from the HAP frame buffer, in which case the socket is not readable and a
 * select would wait for bytes this response will never produce. */
static void set_response_timeout(int fd)
{
    struct timeval timeout;
    timeout.tv_sec = 30;
    timeout.tv_usec = 0;
    /* Best effort: lwIP only honours SO_RCVTIMEO when that option is compiled
     * in, so a failure here means "block until the peer answers". */
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

static void write_raw(writer_t *writer, const void *data, size_t len)
{
    if (!writer->ok || writer->len + len > writer->cap) {
        writer->ok = false;
        return;
    }
    memcpy(writer->out + writer->len, data, len);
    writer->len += len;
}

static void write_str(writer_t *writer, const char *text)
{
    write_raw(writer, text, strlen(text));
}

static void write_format(writer_t *writer, const char *format, ...)
{
    char scratch[96];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(scratch, sizeof(scratch), format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= sizeof(scratch)) {
        writer->ok = false;
        return;
    }
    write_raw(writer, scratch, (size_t)written);
}

void ap_client_attach_fd(ap_client_t *client, int fd)
{
    memset(client, 0, sizeof(*client));
    client->fd = fd;
    hap_channel_init(&client->channel, fd);
    set_response_timeout(fd);
}

int ap_client_connect(ap_client_t *client, const char *host, uint16_t port, uint32_t timeout_ms)
{
    memset(client, 0, sizeof(*client));
    client->fd = ap_socket_connect(host, port, timeout_ms);
    if (client->fd < 0) {
        return -1;
    }
    hap_channel_init(&client->channel, client->fd);
    set_response_timeout(client->fd);
    AP_LOGI("control connection to %s:%u established", host, (unsigned)port);
    return 0;
}

void ap_client_close(ap_client_t *client)
{
    if (client->fd >= 0) {
        ap_socket_close(client->fd);
        client->fd = -1;
    }
}

void ap_client_set_password(ap_client_t *client, const char *password)
{
    client->password = password;
}

void ap_client_enable_encryption(ap_client_t *client, const uint8_t write_key[HAP_KEY_LEN],
                                 const uint8_t read_key[HAP_KEY_LEN])
{
    hap_channel_enable_encryption(&client->channel, write_key, read_key);
    client->encrypted = true;
    AP_LOGI("control channel switched to HAP encryption");
}

size_t ap_client_build(ap_client_t *client, const char *method, const char *uri, const char *content_type,
                       const void *body, size_t body_len, const ap_header_t *headers,
                       const char *authorization, void *out, size_t out_cap)
{
    writer_t writer = { .out = (uint8_t *)out, .cap = out_cap, .len = 0, .ok = true };

    client->cseq += 1;

    write_str(&writer, method);
    write_str(&writer, " ");
    write_str(&writer, uri);
    write_str(&writer, " RTSP/1.0\r\n");
    write_format(&writer, "CSeq: %u\r\n", (unsigned)client->cseq);
    write_str(&writer, "User-Agent: " AP_USER_AGENT "\r\n");
    if (authorization != NULL) {
        write_str(&writer, "Authorization: ");
        write_str(&writer, authorization);
        write_str(&writer, "\r\n");
    }
    if (headers != NULL) {
        for (const ap_header_t *header = headers; header->name != NULL; header++) {
            if (header->value == NULL) {
                continue; /* an unset header is simply not sent */
            }
            write_str(&writer, header->name);
            write_str(&writer, ": ");
            write_str(&writer, header->value);
            write_str(&writer, "\r\n");
        }
    }
    /* Content-Type accompanies a body only; Content-Length is always present so
     * the receiver never has to guess whether a body follows. */
    if (content_type != NULL && body != NULL && body_len > 0) {
        write_str(&writer, "Content-Type: ");
        write_str(&writer, content_type);
        write_str(&writer, "\r\n");
    }
    write_format(&writer, "Content-Length: %u\r\n", (unsigned)body_len);
    write_str(&writer, "\r\n");
    if (body != NULL && body_len > 0) {
        write_raw(&writer, body, body_len);
    }

    return writer.ok ? writer.len : 0;
}

static int read_byte(ap_client_t *client, uint8_t *out)
{
    /* hap_channel_read reads exactly one byte, decrypting through the channel's
     * frame buffer when the connection is encrypted. */
    return hap_channel_read(&client->channel, out, 1);
}

static bool header_name_is(const char *line, size_t name_len, const char *want)
{
    size_t want_len = strlen(want);
    if (name_len != want_len) {
        return false;
    }
    for (size_t i = 0; i < name_len; i++) {
        char a = line[i];
        char b = want[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static bool parse_challenge_params(const char *params, char *realm, size_t realm_cap, char *nonce,
                                   size_t nonce_cap)
{
    bool have_realm = false;
    bool have_nonce = false;
    const char *cursor = params;

    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == ',' || *cursor == '\t') {
            cursor++;
        }
        const char *name = cursor;
        while (*cursor != '\0' && *cursor != '=' && *cursor != ',') {
            cursor++;
        }
        size_t name_len = (size_t)(cursor - name);
        if (*cursor != '=') {
            if (*cursor == '\0') {
                break;
            }
            /* Skip a stray separator or bare token. */
            cursor++;
            continue;
        }
        cursor++;

        char value[128];
        size_t value_len = 0;
        if (*cursor == '"') {
            cursor++;
            while (*cursor != '\0' && *cursor != '"' && value_len + 1 < sizeof(value)) {
                value[value_len++] = *cursor++;
            }
            if (*cursor == '"') {
                cursor++;
            }
        } else {
            while (*cursor != '\0' && *cursor != ',' && value_len + 1 < sizeof(value)) {
                value[value_len++] = *cursor++;
            }
        }
        value[value_len] = '\0';

        if (name_len == 5 && header_name_is(name, name_len, "realm")) {
            snprintf(realm, realm_cap, "%s", value);
            have_realm = true;
        } else if (name_len == 5 && header_name_is(name, name_len, "nonce")) {
            snprintf(nonce, nonce_cap, "%s", value);
            have_nonce = true;
        }
    }
    return have_realm && have_nonce;
}

static void parse_challenge(const char *value, ap_response_t *response)
{
    const char *space = strchr(value, ' ');
    if (space == NULL) {
        return;
    }
    response->has_challenge =
        parse_challenge_params(space + 1, response->realm, sizeof(response->realm), response->nonce,
                               sizeof(response->nonce));
}

static bool parse_content_length(const char *text, size_t len, size_t *out)
{
    if (len == 0) {
        return false;
    }
    size_t value = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + (size_t)(text[i] - '0');
        if (value > AP_BODY_MAX) {
            return false;
        }
    }
    *out = value;
    return true;
}

static bool parse_decimal(const char *text, size_t len, uint64_t *out)
{
    uint64_t value = 0;

    if (len == 0 || len > 20) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + (uint64_t)(text[i] - '0');
    }
    *out = value;
    return true;
}

static int read_response(ap_client_t *client, ap_response_t *response)
{
    char header[AP_HEADER_MAX];
    size_t header_len = 0;

    memset(response, 0, sizeof(*response));
    client->has_clock_headers = false;
    client->received_timestamp_ms = 0;
    client->processing_time_ms = 0;

    for (;;) {
        uint8_t byte;
        if (read_byte(client, &byte) != 0) {
            AP_LOGE("read response header failed after %u bytes", (unsigned)header_len);
            return -1;
        }
        if (header_len + 1 >= sizeof(header)) {
            AP_LOGE("response header exceeds %u bytes", (unsigned)sizeof(header));
            return -1;
        }
        header[header_len++] = (char)byte;
        if (header_len >= 4 && memcmp(header + header_len - 4, "\r\n\r\n", 4) == 0) {
            break;
        }
    }
    header[header_len] = '\0';

    const char *line_end = strstr(header, "\r\n");
    if (line_end == NULL) {
        AP_LOGE("malformed status line");
        return -1;
    }
    const char *space = memchr(header, ' ', (size_t)(line_end - header));
    if (space == NULL) {
        AP_LOGE("malformed status line");
        return -1;
    }
    response->status = atoi(space + 1);

    size_t content_length = 0;
    bool has_content_length = false;
    uint64_t received_ms = 0;
    uint64_t processing_ms = 0;
    bool has_received = false;
    bool has_processing = false;

    const char *line = line_end + 2;
    while (*line != '\0') {
        const char *next = strstr(line, "\r\n");
        if (next == NULL || next == line) {
            break;
        }
        const char *colon = memchr(line, ':', (size_t)(next - line));
        if (colon != NULL) {
            size_t name_len = (size_t)(colon - line);
            const char *value = colon + 1;
            while (value < next && (*value == ' ' || *value == '\t')) {
                value++;
            }
            size_t value_len = (size_t)(next - value);

            if (header_name_is(line, name_len, "content-length")) {
                if (!parse_content_length(value, value_len, &content_length)) {
                    AP_LOGE("invalid Content-Length in response");
                    return -1;
                }
                has_content_length = true;
            } else if (header_name_is(line, name_len, "www-authenticate")) {
                char value_buf[192];
                size_t copy = value_len < sizeof(value_buf) - 1 ? value_len : sizeof(value_buf) - 1;
                memcpy(value_buf, value, copy);
                value_buf[copy] = '\0';
                parse_challenge(value_buf, response);
            } else if (header_name_is(line, name_len, "x-apple-requestreceivedtimestamp")) {
                has_received = parse_decimal(value, value_len, &received_ms);
            } else if (header_name_is(line, name_len, "x-apple-processingtime")) {
                has_processing = parse_decimal(value, value_len, &processing_ms);
            }
        }
        line = next + 2;
    }

    if (!has_content_length) {
        /* A response without Content-Length carries no body; the connection
         * cannot delimit one any other way. */
        content_length = 0;
    }

    client->has_clock_headers = has_received && has_processing;
    client->received_timestamp_ms = received_ms;
    client->processing_time_ms = processing_ms;

    client->body_len = 0;
    if (content_length > 0) {
        if (content_length > sizeof(client->body)) {
            AP_LOGE("response body %u bytes exceeds the %u byte buffer", (unsigned)content_length,
                    (unsigned)sizeof(client->body));
            return -1;
        }
        if (hap_channel_read(&client->channel, client->body, content_length) != 0) {
            AP_LOGE("read response body failed (%u bytes)", (unsigned)content_length);
            return -1;
        }
        client->body_len = content_length;
    }
    response->body_len = client->body_len;

    return 0;
}

static void digest_authorization(ap_client_t *client, const char *method, const char *uri, char *out,
                                 size_t cap)
{
    char input[256];
    char ha1[33];
    char ha2[33];
    char response[33];

    snprintf(input, sizeof(input), "%s:%s:%s", AP_DIGEST_USERNAME, client->realm,
             client->password != NULL ? client->password : "");
    ap_md5_hex(input, strlen(input), ha1);

    snprintf(input, sizeof(input), "%s:%s", method, uri);
    ap_md5_hex(input, strlen(input), ha2);

    snprintf(input, sizeof(input), "%s:%s:%s", ha1, client->nonce, ha2);
    ap_md5_hex(input, strlen(input), response);

    snprintf(out, cap,
             "Digest username=\"" AP_DIGEST_USERNAME "\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
             "response=\"%s\"",
             client->realm, client->nonce, uri, response);
}

int ap_client_request(ap_client_t *client, const char *method, const char *uri, const char *content_type,
                      const void *body, size_t body_len, const ap_header_t *headers,
                      ap_response_t *response)
{
    ap_response_t local;
    if (response == NULL) {
        response = &local;
    }
    memset(response, 0, sizeof(*response));
    if (body == NULL) {
        body_len = 0;
    }

    bool authenticated = client->has_challenge;
    for (int attempt = 0; attempt < 2; attempt++) {
        char authorization[384];
        const char *authorization_ptr = NULL;
        if (authenticated && client->password != NULL) {
            digest_authorization(client, method, uri, authorization, sizeof(authorization));
            authorization_ptr = authorization;
        }

        size_t request_len = ap_client_build(client, method, uri, content_type, body, body_len, headers,
                                             authorization_ptr, client->request, sizeof(client->request));
        if (request_len == 0) {
            AP_LOGE("%s %s: request does not fit the %u byte buffer", method, uri,
                    (unsigned)sizeof(client->request));
            return -1;
        }
        if (hap_channel_write(&client->channel, client->request, request_len) != 0) {
            AP_LOGE("%s %s: write failed", method, uri);
            return -1;
        }
        AP_LOGI("-> %s %s (body=%u, cseq=%u%s)", method, uri, (unsigned)body_len, (unsigned)client->cseq,
                client->encrypted ? ", encrypted" : "");

        if (read_response(client, response) != 0) {
            return -1;
        }

        if (response->status == 401 && response->has_challenge && !authenticated &&
            client->password != NULL) {
            client->has_challenge = true;
            snprintf(client->realm, sizeof(client->realm), "%s", response->realm);
            snprintf(client->nonce, sizeof(client->nonce), "%s", response->nonce);
            AP_LOGW("%s %s: 401 Digest challenge, retrying authenticated", method, uri);
            authenticated = true;
            continue;
        }
        break;
    }

    if (response->status < 200 || response->status >= 300) {
        AP_LOGE("%s %s failed: status %d, body %u bytes", method, uri, response->status,
                (unsigned)response->body_len);
        if (response->body_len > 0) {
            ap_log_hex("response body", client->body, response->body_len);
        }
        return -1;
    }

    AP_LOGI("<- %d (%u body bytes)", response->status, (unsigned)response->body_len);
    return 0;
}

const void *ap_client_body(const ap_client_t *client, size_t *len)
{
    if (len != NULL) {
        *len = client->body_len;
    }
    return client->body;
}
