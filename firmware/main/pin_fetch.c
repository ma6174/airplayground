#include "pin_fetch.h"

#include <string.h>

#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "pin_fetch";

/* One line every few attempts: otherwise a silent fetch has no visibility. */
static void report(const char *stage)
{
    static unsigned attempts;
    if ((attempts++ % 5) == 0) {
        ESP_LOGW(TAG, "fetch stage: %s", stage);
    }
}

/* Pulls the first run of four digits out of an HTTP response body. */
static bool digits_from_body(const char *body, char pin[5])
{
    int digits = 0;

    for (const char *cursor = body; *cursor != '\0'; cursor++) {
        if (*cursor >= '0' && *cursor <= '9') {
            pin[digits++] = *cursor;
            if (digits == 4) {
                pin[4] = '\0';
                return true;
            }
        } else if (digits != 0) {
            digits = 0; /* a run only counts when it is exactly four long */
        }
    }
    return false;
}

bool pin_fetch_from_host(const char *host, uint16_t port, char pin[5])
{
    if (host == NULL || host[0] == '\0' || pin == NULL) {
        return false;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = inet_addr(host);
    if (address.sin_addr.s_addr == INADDR_NONE) {
        return false;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return false;
    }
    /* lwIP only honours SO_RCVTIMEO when it is compiled in, so reads are
     * guarded with select() instead of relying on the socket option. */

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        report("connect failed");
        return false;
    }

    char request[128];
    int length = snprintf(request, sizeof(request),
                          "GET /pin HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    if (length <= 0 || write(fd, request, (size_t)length) != length) {
        close(fd);
        report("request write failed");
        return false;
    }

    char response[640];
    size_t used = 0;
    while (used + 1 < sizeof(response)) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        if (select(fd + 1, &readable, NULL, NULL, &timeout) <= 0) {
            break;
        }
        ssize_t got = read(fd, response + used, sizeof(response) - 1 - used);
        if (got <= 0) {
            break;
        }
        used += (size_t)got;
    }
    response[used] = '\0';
    close(fd);

    char *body = strstr(response, "\r\n\r\n");
    if (body == NULL) {
        report("no header terminator in the response");
        return false;
    }
    if (digits_from_body(body + 4, pin)) {
        ESP_LOGI(TAG, "pairing code received from %s:%u", host, (unsigned)port);
        return true;
    }
    report("response carried no 4-digit code");
    return false;
}
