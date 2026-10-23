#include "ap_event.h"

#include <stdio.h>
#include <string.h>

#include "ap_internal.h"

static const char *TAG = "airplay";

#define AP_EVENT_CONNECT_TIMEOUT_MS 3000
#define AP_EVENT_LINE_MAX 192
#define AP_EVENT_BODY_MAX 1024

static bool name_is(const char *text, const char *want)
{
    while (*text != '\0' && *want != '\0') {
        char a = *text++;
        char b = *want++;
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return *text == '\0' && *want == '\0';
}

/* Reads one CRLF-terminated line. Returns its length, or -1 when the channel
 * closed or failed. */
static int read_line(hap_channel_t *channel, char *out, size_t cap)
{
    size_t len = 0;
    for (;;) {
        uint8_t byte;
        if (hap_channel_read(channel, &byte, 1) != 0) {
            return -1;
        }
        if (byte == '\n') {
            break;
        }
        if (len + 1 < cap) {
            out[len++] = (char)byte;
        }
    }
    if (len > 0 && out[len - 1] == '\r') {
        len--;
    }
    out[len] = '\0';
    return (int)len;
}

static void event_task(void *argument)
{
    ap_session_t *session = (ap_session_t *)argument;
    ap_event_channel_t *event = &session->event;

    char request_line[AP_EVENT_LINE_MAX];
    char header_line[AP_EVENT_LINE_MAX];
    char method[16];
    char path[128];
    char version[16];

    while (!session->stop) {
        int line_len = read_line(&event->channel, request_line, sizeof(request_line));
        if (line_len < 0) {
            break;
        }
        if (line_len == 0) {
            continue;
        }

        method[0] = '\0';
        path[0] = '\0';
        version[0] = '\0';
        sscanf(request_line, "%15s %127s %15s", method, path, version);

        uint32_t cseq = 0;
        size_t content_length = 0;
        for (;;) {
            line_len = read_line(&event->channel, header_line, sizeof(header_line));
            if (line_len <= 0) {
                break;
            }
            char *colon = strchr(header_line, ':');
            if (colon == NULL) {
                continue;
            }
            *colon = '\0';
            char *value = colon + 1;
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            if (name_is(header_line, "content-length")) {
                content_length = (size_t)strtoul(value, NULL, 10);
            } else if (name_is(header_line, "cseq")) {
                cseq = (uint32_t)strtoul(value, NULL, 10);
            }
        }

        uint8_t body[AP_EVENT_BODY_MAX];
        size_t body_read = 0;
        while (body_read < content_length) {
            size_t chunk = content_length - body_read;
            if (chunk > sizeof(body)) {
                chunk = sizeof(body);
            }
            if (hap_channel_read(&event->channel, body, chunk) != 0) {
                break;
            }
            body_read += chunk;
        }

        AP_LOGI("event request: %s %s (cseq=%u, body=%u bytes)", method, path, (unsigned)cseq,
                (unsigned)content_length);

        char response[96];
        int response_len = snprintf(response, sizeof(response),
                                    "RTSP/1.0 200 OK\r\nCSeq: %u\r\nContent-Length: 0\r\n\r\n",
                                    (unsigned)cseq);
        if (response_len > 0 &&
            hap_channel_write(&event->channel, response, (size_t)response_len) != 0) {
            break;
        }
    }

    event->active = false;
    AP_LOGW("event channel reader stopped");
    vTaskDelete(NULL);
}

int ap_event_start(ap_session_t *session, uint16_t port)
{
    if (port == 0) {
        return -1;
    }

    memset(&session->event, 0, sizeof(session->event));
    session->event.fd = -1;

    int fd = ap_socket_connect(session->config->host, port, AP_EVENT_CONNECT_TIMEOUT_MS);
    if (fd < 0) {
        AP_LOGW("could not connect to the receiver event port %u", (unsigned)port);
        return -1;
    }
    session->event.fd = fd;
    hap_channel_init(&session->event.channel, fd);

    if (session->client.encrypted) {
        uint8_t events_write[HAP_KEY_LEN];
        uint8_t events_read[HAP_KEY_LEN];
        /* Reversed relative to the control channel: commands arrive under the
         * Events-Write key and our acknowledgements leave under Events-Read, so
         * the derivation already hands back this channel's own direction. */
        hap_derive_event_keys(session->shared, events_write, events_read);
        hap_channel_enable_encryption(&session->event.channel, events_write, events_read);
    }
    session->event.active = true;

    if (xTaskCreate(event_task, "ap_event", 8192, session, 4, &session->event_task) != pdPASS) {
        AP_LOGE("could not start the event channel task");
        ap_socket_close(fd);
        session->event.fd = -1;
        session->event.active = false;
        return -1;
    }

    AP_LOGI("event channel connected to port %u", (unsigned)port);
    return 0;
}

void ap_event_stop(ap_session_t *session)
{
    session->event.active = false;
    /* Only the session task closes this socket. A reader that exits early just
     * stops reading and leaves the descriptor to teardown, which keeps the
     * close path single-owner. */
    if (session->event.fd >= 0) {
        ap_socket_close(session->event.fd);
        session->event.fd = -1;
    }
    session->event_task = NULL;
}
