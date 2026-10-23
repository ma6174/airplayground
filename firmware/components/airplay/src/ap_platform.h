/*
 * The two things the AirPlay sender needs from its host: sockets and a clock.
 *
 * ESP-IDF and the host-test build both expose a BSD socket API, so only the
 * includes, logging and the monotonic clock differ. Everything above this
 * header is written once against these helpers, which keeps the protocol code
 * (plist, RTSP framing, frame headers) compilable and testable off-device.
 */
#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#define AP_LOGI(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define AP_LOGW(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#define AP_LOGE(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)

#else /* host test build */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

void ap_host_log(char level, const char *tag, const char *fmt, ...);

#define AP_LOGI(fmt, ...) ap_host_log('I', TAG, fmt, ##__VA_ARGS__)
#define AP_LOGW(fmt, ...) ap_host_log('W', TAG, fmt, ##__VA_ARGS__)
#define AP_LOGE(fmt, ...) ap_host_log('E', TAG, fmt, ##__VA_ARGS__)

#endif

/* Monotonic milliseconds / microseconds since boot. */
uint64_t ap_millis(void);
uint64_t ap_micros(void);

/* Fills out with len bytes of entropy. */
void ap_random_bytes(uint8_t *out, size_t len);

/* Blocking TCP connect with a deadline. Returns a connected fd or -1. */
int ap_socket_connect(const char *host, uint16_t port, uint32_t timeout_ms);

/* Binds a UDP socket to an ephemeral port and reports which one. Returns fd or -1. */
int ap_socket_udp_open(uint16_t *port_out);

/* Waits until fd is readable (writable == false) or writable.
 * Returns 1 when ready, 0 on timeout, -1 on error. */
int ap_socket_wait(int fd, bool writable, uint32_t timeout_ms);

/* Writes len bytes, honouring the deadline for each syscall. 0 on success. */
int ap_socket_write_all(int fd, const void *data, size_t len);

void ap_socket_close(int fd);
int ap_socket_set_nodelay(int fd);

/* Logs up to 64 bytes of data as hex, for diagnosing rejected replies. */
void ap_log_hex(const char *label, const void *data, size_t len);
