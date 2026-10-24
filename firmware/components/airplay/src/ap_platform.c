#include "ap_platform.h"

#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

static const char *TAG = "airplay";

#ifdef ESP_PLATFORM

uint64_t ap_millis(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

uint64_t ap_micros(void)
{
    return (uint64_t)esp_timer_get_time();
}

void ap_random_bytes(uint8_t *out, size_t len)
{
    esp_fill_random(out, len);
}

#else

void ap_host_log(char level, const char *tag, const char *fmt, ...)
{
    va_list args;
    printf("[%c] %s: ", level, tag);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    putchar('\n');
    fflush(stdout);
}

uint64_t ap_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t ap_micros(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void ap_random_bytes(uint8_t *out, size_t len)
{
    arc4random_buf(out, len);
}

#endif

int ap_socket_wait(int fd, bool writable, uint32_t timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);

    struct timeval tv;
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);

    int rc;
    if (writable) {
        rc = select(fd + 1, NULL, &set, NULL, &tv);
    } else {
        rc = select(fd + 1, &set, NULL, NULL, &tv);
    }
    if (rc > 0) {
        return 1;
    }
    if (rc == 0) {
        return 0;
    }
    return -1;
}

int ap_socket_connect(const char *host, uint16_t port, uint32_t timeout_ms)
{
    char service[8];
    snprintf(service, sizeof(service), "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, service, &hints, &res) != 0 || res == NULL) {
        AP_LOGE("resolve %s:%u failed", host, (unsigned)port);
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
            ap_socket_close(fd);
            fd = -1;
            continue;
        }
        if (rc != 0) {
            if (ap_socket_wait(fd, true, timeout_ms) != 1) {
                ap_socket_close(fd);
                fd = -1;
                continue;
            }
            /* getpeername only succeeds once the handshake completed; this
             * avoids relying on SO_ERROR, which not every lwIP build reports. */
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            if (getpeername(fd, (struct sockaddr *)&peer, &peer_len) != 0) {
                ap_socket_close(fd);
                fd = -1;
                continue;
            }
        }

        fcntl(fd, F_SETFL, flags);
        break;
    }

    freeaddrinfo(res);
    return fd;
}

int ap_socket_send_to(int fd, const char *host, uint16_t port, const void *data, size_t len)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    char service[8];
    snprintf(service, sizeof(service), "%u", (unsigned)port);

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, service, &hints, &res) != 0 || res == NULL) {
        return -1;
    }
    int rc = sendto(fd, data, len, 0, res->ai_addr, res->ai_addrlen) == (ssize_t)len ? 0 : -1;
    freeaddrinfo(res);
    return rc;
}

int ap_socket_udp_open(uint16_t *port_out)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ap_socket_close(fd);
        return -1;
    }

    socklen_t addr_len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        ap_socket_close(fd);
        return -1;
    }
    if (port_out != NULL) {
        *port_out = ntohs(addr.sin_port);
    }
    return fd;
}

int ap_socket_write_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        int ready = ap_socket_wait(fd, true, 2000);
        if (ready != 1) {
            return -1;
        }
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

void ap_socket_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}

int ap_socket_set_nodelay(int fd)
{
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

void ap_log_hex(const char *label, const void *data, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    const uint8_t *bytes = (const uint8_t *)data;
    size_t shown = len > 64 ? 64 : len;
    char line[3 * 32 + 1];

    AP_LOGE("%s (%u bytes)%s", label, (unsigned)len, len > shown ? ", first 64:" : ":");
    for (size_t off = 0; off < shown; off += 32) {
        size_t chunk = shown - off;
        if (chunk > 32) {
            chunk = 32;
        }
        for (size_t i = 0; i < chunk; i++) {
            line[i * 3] = hex[bytes[off + i] >> 4];
            line[i * 3 + 1] = hex[bytes[off + i] & 0x0f];
            line[i * 3 + 2] = ' ';
        }
        line[chunk * 3] = '\0';
        AP_LOGE("%04x: %s", (unsigned)off, line);
    }
}
