/* Host tests for the control channel: request bytes, response parsing (plain
 * and fragmented), Digest retry, and error propagation.
 *
 * A socketpair plus a scripted server thread stands in for the receiver, so the
 * parser is driven by the same byte stream a real TCP connection would deliver.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "ap_client.h"
#include "ap_md5.h"

static int failures;

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) {                                               \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                              \
        }                                                            \
    } while (0)

static char g_request[2048];
static char g_retry_request[2048];
static int g_server_error;

static void set_timeout(int fd, int seconds)
{
    struct timeval timeout = { .tv_sec = seconds, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

/* Reads one RTSP request header block from the peer. */
static size_t read_request(int fd, char *buffer, size_t capacity)
{
    size_t len = 0;
    while (len + 1 < capacity) {
        char byte;
        ssize_t received = recv(fd, &byte, 1, 0);
        if (received <= 0) {
            break;
        }
        buffer[len++] = byte;
        if (len >= 4 && memcmp(buffer + len - 4, "\r\n\r\n", 4) == 0) {
            break;
        }
    }
    buffer[len] = '\0';
    return len;
}

static void send_all(int fd, const char *text)
{
    size_t len = strlen(text);
    while (len > 0) {
        ssize_t sent = send(fd, text, len, 0);
        if (sent <= 0) {
            g_server_error = 1;
            return;
        }
        text += sent;
        len -= (size_t)sent;
    }
}

static void *server_fragmented(void *argument)
{
    int fd = *(int *)argument;
    set_timeout(fd, 3);
    read_request(fd, g_request, sizeof(g_request));

    const char *response = "RTSP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    /* One byte per write: the parser must not assume a response arrives whole. */
    for (size_t i = 0; i < strlen(response); i++) {
        if (send(fd, response + i, 1, 0) != 1) {
            g_server_error = 1;
            break;
        }
        usleep(200);
    }
    close(fd);
    return NULL;
}

static void *server_http11(void *argument)
{
    int fd = *(int *)argument;
    set_timeout(fd, 3);
    read_request(fd, g_request, sizeof(g_request));
    send_all(fd, "HTTP/1.1 200 OK\r\ncOnTeNt-LeNgTh: 0\r\nServer: AirTunes/935.7.1\r\n\r\n");
    close(fd);
    return NULL;
}

static void *server_digest(void *argument)
{
    int fd = *(int *)argument;
    set_timeout(fd, 3);

    read_request(fd, g_request, sizeof(g_request));
    send_all(fd,
             "RTSP/1.0 401 Unauthorized\r\n"
             "Content-Length: 0\r\n"
             "WWW-Authenticate: Digest realm=\"airplay\", nonce=\"abc123\"\r\n"
             "\r\n");

    read_request(fd, g_retry_request, sizeof(g_retry_request));
    send_all(fd, "RTSP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok");
    close(fd);
    return NULL;
}

static void *server_not_found(void *argument)
{
    int fd = *(int *)argument;
    set_timeout(fd, 3);
    read_request(fd, g_request, sizeof(g_request));
    send_all(fd, "RTSP/1.0 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 3\r\n\r\nno!");
    close(fd);
    return NULL;
}

static void test_request_bytes(void)
{
    printf("test_request_bytes\n");
    ap_client_t client;
    memset(&client, 0, sizeof(client));

    uint8_t buffer[512];
    size_t len = ap_client_build(&client, "GET", "/info", NULL, NULL, 0, NULL, NULL, buffer,
                                 sizeof(buffer));
    const char *expected =
        "GET /info RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "User-Agent: AirPlay/935.7.1\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    CHECK(len == strlen(expected));
    CHECK(memcmp(buffer, expected, len) == 0);
    if (len != strlen(expected) || memcmp(buffer, expected, len) != 0) {
        printf("  got: %.*s\n", (int)len, buffer);
    }

    /* CSeq advances per request on the connection. */
    len = ap_client_build(&client, "GET", "/info", NULL, NULL, 0, NULL, NULL, buffer, sizeof(buffer));
    CHECK(strstr((char *)buffer, "CSeq: 2\r\n") != NULL);

    /* Body present: Content-Type appears between the extra headers and the
     * length; Content-Length always appears. */
    const uint8_t body[] = { 0x01, 0x02, 0x03 };
    ap_header_t headers[] = { { "X-Apple-HKP", "4" }, { NULL, NULL } };
    len = ap_client_build(&client, "POST", "/pair-setup", "application/octet-stream", body,
                          sizeof(body), headers, NULL, buffer, sizeof(buffer));
    const char *expected_post =
        "POST /pair-setup RTSP/1.0\r\n"
        "CSeq: 3\r\n"
        "User-Agent: AirPlay/935.7.1\r\n"
        "X-Apple-HKP: 4\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: 3\r\n"
        "\r\n";
    CHECK(len == strlen(expected_post) + sizeof(body));
    CHECK(memcmp(buffer, expected_post, strlen(expected_post)) == 0);
    CHECK(memcmp(buffer + strlen(expected_post), body, sizeof(body)) == 0);

    /* No body: the content type must be omitted even when one is supplied. */
    len = ap_client_build(&client, "POST", "/pair-pin-start", "application/octet-stream", NULL, 0, NULL,
                          NULL, buffer, sizeof(buffer));
    CHECK(strstr((char *)buffer, "Content-Type") == NULL);
    CHECK(strstr((char *)buffer, "Content-Length: 0\r\n") != NULL);

    /* A request that does not fit is rejected rather than truncated. */
    CHECK(ap_client_build(&client, "GET", "/info", NULL, NULL, 0, NULL, NULL, buffer, 20) == 0);
}

static void test_md5(void)
{
    printf("test_md5\n");
    char hex[33];
    ap_md5_hex("", 0, hex);
    CHECK(strcmp(hex, "d41d8cd98f00b204e9800998ecf8427e") == 0);
    ap_md5_hex("abc", 3, hex);
    CHECK(strcmp(hex, "900150983cd24fb0d6963f7d28e17f72") == 0);
    /* Multi-block input. */
    ap_md5_hex("The quick brown fox jumps over the lazy dog", 43, hex);
    CHECK(strcmp(hex, "9e107d9d372bb6826bd81d3542a419d6") == 0);
}

static void test_fragmented_response(void)
{
    printf("test_fragmented_response\n");
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    g_server_error = 0;

    pthread_t thread;
    pthread_create(&thread, NULL, server_fragmented, &fds[1]);

    ap_client_t client;
    ap_client_attach_fd(&client, fds[0]);
    ap_response_t response;
    int rc = ap_client_request(&client, "GET", "/info", NULL, NULL, 0, NULL, &response);
    CHECK(rc == 0);
    CHECK(response.status == 200);
    size_t body_len = 0;
    const void *body = ap_client_body(&client, &body_len);
    CHECK(body_len == 5);
    CHECK(body_len == 5 && memcmp(body, "hello", 5) == 0);

    pthread_join(thread, NULL);
    CHECK(g_server_error == 0);
    ap_client_close(&client);
}

static void test_http11_status_line(void)
{
    printf("test_http11_status_line\n");
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    pthread_t thread;
    pthread_create(&thread, NULL, server_http11, &fds[1]);

    ap_client_t client;
    ap_client_attach_fd(&client, fds[0]);
    ap_response_t response;
    int rc = ap_client_request(&client, "GET", "/info", NULL, NULL, 0, NULL, &response);
    CHECK(rc == 0);
    CHECK(response.status == 200);
    size_t body_len = 1;
    ap_client_body(&client, &body_len);
    CHECK(body_len == 0);

    pthread_join(thread, NULL);
    ap_client_close(&client);
}

static void test_digest_retry(void)
{
    printf("test_digest_retry\n");
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    memset(g_retry_request, 0, sizeof(g_retry_request));

    pthread_t thread;
    pthread_create(&thread, NULL, server_digest, &fds[1]);

    ap_client_t client;
    ap_client_attach_fd(&client, fds[0]);
    ap_client_set_password(&client, "secret");
    ap_response_t response;
    int rc = ap_client_request(&client, "GET", "/info", NULL, NULL, 0, NULL, &response);
    CHECK(rc == 0);
    CHECK(response.status == 200);

    pthread_join(thread, NULL);

    const char *expected =
        "Authorization: Digest username=\"AirPlay\", realm=\"airplay\", nonce=\"abc123\", "
        "uri=\"/info\", response=\"d1b2dfda4c72f5d00bbf8da194ee0e8a\"";
    if (strstr(g_retry_request, expected) == NULL) {
        printf("  retry request was:\n%s\n", g_retry_request);
    }
    CHECK(strstr(g_retry_request, expected) != NULL);
    CHECK(strstr(g_request, "Authorization") == NULL);
    ap_client_close(&client);
}

static void test_error_status(void)
{
    printf("test_error_status\n");
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    pthread_t thread;
    pthread_create(&thread, NULL, server_not_found, &fds[1]);

    ap_client_t client;
    ap_client_attach_fd(&client, fds[0]);
    ap_response_t response;
    int rc = ap_client_request(&client, "GET", "/info", NULL, NULL, 0, NULL, &response);
    CHECK(rc != 0);
    CHECK(response.status == 404);
    size_t body_len = 0;
    const void *body = ap_client_body(&client, &body_len);
    CHECK(body_len == 3);
    CHECK(body_len == 3 && memcmp(body, "no!", 3) == 0);

    pthread_join(thread, NULL);
    ap_client_close(&client);
}

int main(void)
{
    test_md5();
    test_request_bytes();
    test_fragmented_response();
    test_http11_status_line();
    test_digest_retry();
    test_error_status();

    if (failures != 0) {
        printf("test_client: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_client: ok\n");
    return 0;
}
