#include "media_net.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lwip/sockets.h"

#include "media_avcc.h"

static const char *TAG = "media_net";

/* Wire format, all fields big-endian:
 *
 *   CONFIG  magic 'APVN' | type 1 | version 1 | reserved 2 | width 2 |
 *           height 2 | fps 2 | sps_len 2 | pps_len 2 | sps | pps
 *   FRAME   magic 'APVN' | type 2 | version 1 | flags 1 | reserved 2 |
 *           au_len 4 | pts_ms 4 | AVCC access unit
 *
 * flags bit 0 marks an IDR access unit. Every access unit carries its own
 * boundaries, so the receiving side never parses H.264 to split TCP reads. */
#define NET_MAGIC 0x4150564eu
#define NET_VERSION 1
#define NET_TYPE_CONFIG 1
#define NET_TYPE_FRAME 2
#define NET_FLAG_IDR 0x01u
#define NET_MAX_AU (512u * 1024u)
/* A frame pointer that is absurdly large means the stream lost sync, not that
 * the encoder went wild; only then is dropping the connection the right move. */
#define NET_MAX_DRAIN (4u * 1024u * 1024u)
#define NET_MAX_PARAM 1024u
#define NET_AVCC_CAP (NET_MAX_PARAM * 2 + 64)
#define NET_HEADER_MAX 32u

/* Frames are queued instead of overwritten: an H.264 stream breaks the moment a
 * P frame goes missing, and the receiver cannot recover until the next IDR.
 * The consumer holds one slot while it encrypts and sends it, so the depth is
 * one more than the queue. The depth has to swallow a keyframe: a 26 KB IDR
 * costs roughly 150 ms on the mirror channel, during which the encoder keeps
 * producing frames at the capture rate.
 *
 * The producer pushes back rather than dropping: TCP's own flow control then
 * slows the macOS encoder down, which keeps the reference chain intact. Only a
 * stall longer than NET_PUSH_TIMEOUT_MS drops the oldest frame, and the client
 * sends a fresh IDR soon enough (see its keyframe interval) to resynchronise. */
#define NET_QUEUE_DEPTH 8
#define NET_SLOTS (NET_QUEUE_DEPTH + 1)
#define NET_PUSH_TIMEOUT_MS 300
/* No session drains the queue between /start calls, but the capture client
 * keeps streaming. Recognising that quiet consumer keeps the queue from being
 * churned (and logged) frame by frame until the next session begins. */
#define NET_CONSUMER_IDLE_MS 500
#define NET_PUSH_POLL_MS 5
/* A client that vanishes without closing its socket (killed while the link is
 * down, or a machine that went to sleep) leaves recv() blocked forever. Without
 * a deadline the capture task never returns to accept(), and the port stops
 * listening - no new client can connect until the device is reset. */
#define NET_CLIENT_IDLE_MS 30000

static struct {
    uint16_t port;
    uint16_t fps;
    volatile bool configured;
    bool started;
    uint8_t *avcc;
    size_t avcc_len;
    uint8_t *slots[NET_SLOTS];
    size_t slot_len[NET_SLOTS];
    bool slot_idr[NET_SLOTS];
    TickType_t last_consume;
    /* Frames the session could not use: the queue overflowed, versus frames
     * skipped while waiting for an IDR after the session began. */
    uint32_t dropped_overflow;
    /* Index of the oldest queued frame and how many are queued. */
    uint8_t head;
    uint8_t count;
    /* Set when a session starts: the receiver has no reference picture yet, so
     * the queue is skipped until an IDR arrives. */
    bool need_idr;
    uint32_t dropped;
    uint32_t oversized;
    SemaphoreHandle_t queued;
    SemaphoreHandle_t lock;
} s_net;

static airplay_live_source_t s_source;

static void *big_alloc(size_t len)
{
    void *block = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (block != NULL) {
        return block;
    }
    return heap_caps_malloc(len, MALLOC_CAP_DEFAULT);
}

static bool recv_ready(int fd, uint32_t timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = {
        .tv_sec = (time_t)(timeout_ms / 1000),
        .tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000),
    };
    return select(fd + 1, &set, NULL, NULL, &tv) > 0;
}

static bool recv_all(int fd, void *buf, size_t len)
{
    uint8_t *cursor = buf;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(NET_CLIENT_IDLE_MS);
    while (len != 0) {
        if (!recv_ready(fd, 1000)) {
            if (xTaskGetTickCount() > deadline) {
                ESP_LOGW(TAG, "capture client went quiet, dropping it");
                return false;
            }
            continue;
        }
        int got = recv(fd, cursor, len, 0);
        if (got <= 0) {
            return false;
        }
        cursor += got;
        len -= (size_t)got;
        deadline = xTaskGetTickCount() + pdMS_TO_TICKS(NET_CLIENT_IDLE_MS);
    }
    return true;
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static bool handle_config(int fd)
{
    uint8_t header[NET_HEADER_MAX];
    if (!recv_all(fd, header, 18) || read_be32(header) != NET_MAGIC ||
        header[4] != NET_TYPE_CONFIG || header[5] != NET_VERSION) {
        ESP_LOGW(TAG, "malformed configure packet");
        return false;
    }

    uint16_t width = read_be16(header + 8);
    uint16_t height = read_be16(header + 10);
    uint16_t fps = read_be16(header + 12);
    uint16_t sps_len = read_be16(header + 14);
    uint16_t pps_len = read_be16(header + 16);
    if (width == 0 || height == 0 || fps == 0 || sps_len < 4 || pps_len == 0 ||
        sps_len > NET_MAX_PARAM || pps_len > NET_MAX_PARAM) {
        ESP_LOGW(TAG, "configure packet out of range");
        return false;
    }

    uint8_t sps[NET_MAX_PARAM];
    uint8_t pps[NET_MAX_PARAM];
    if (!recv_all(fd, sps, sps_len) || !recv_all(fd, pps, pps_len)) {
        return false;
    }
    size_t avcc_len = media_avcc_record(sps, sps_len, pps, pps_len, s_net.avcc, NET_AVCC_CAP);
    if (avcc_len == 0) {
        ESP_LOGW(TAG, "codec record does not fit");
        return false;
    }

    s_net.configured = false;
    s_net.fps = fps;
    s_net.avcc_len = avcc_len;
    /* The session reads the geometry from the source, so publish it here. */
    s_source.width = width;
    s_source.height = height;
    s_net.configured = true;
    ESP_LOGI(TAG, "capture configured: %ux%u @ %u fps", (unsigned)width, (unsigned)height,
             (unsigned)fps);
    return true;
}

/* Reads an oversized frame off the socket so the stream stays in sync, then
 * throws it away. */
static bool drain_frame(int fd, uint32_t len)
{
    uint8_t scratch[256];
    while (len != 0) {
        size_t chunk = len < sizeof(scratch) ? len : sizeof(scratch);
        if (!recv_all(fd, scratch, chunk)) {
            return false;
        }
        len -= (uint32_t)chunk;
    }
    return true;
}

static bool handle_frame(int fd)
{
    uint8_t header[NET_HEADER_MAX];
    if (!recv_all(fd, header, 16) || read_be32(header) != NET_MAGIC ||
        header[4] != NET_TYPE_FRAME || header[5] != NET_VERSION) {
        ESP_LOGW(TAG, "malformed frame packet");
        return false;
    }
    uint32_t au_len = read_be32(header + 8);
    if (au_len == 0 || au_len > NET_MAX_DRAIN) {
        ESP_LOGW(TAG, "access unit of %u bytes is out of range", (unsigned)au_len);
        return false;
    }
    /* Text-heavy windows spike their keyframes far above the average bitrate.
     * Skipping one oversized frame costs at most one keyframe period of
     * stutter, while closing the connection would take the whole stream down.
     * The frame still has to be read off the socket to keep it in sync. */
    if (au_len > NET_MAX_AU) {
        s_net.oversized++;
        if (s_net.oversized % 30 == 1) {
            ESP_LOGW(TAG, "skipping %u-byte frame, over the %u-byte limit (%u skipped)",
                     (unsigned)au_len, (unsigned)NET_MAX_AU, (unsigned)s_net.oversized);
        }
        return drain_frame(fd, au_len);
    }

    /* Wait for room, pushing back on the socket by not reading meanwhile. */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(NET_PUSH_TIMEOUT_MS);
    for (;;) {
        xSemaphoreTake(s_net.lock, portMAX_DELAY);
        bool room = s_net.count < NET_QUEUE_DEPTH;
        if (!room && xTaskGetTickCount() > deadline) {
            /* The consumer has stalled: give up the oldest frame so the client
             * keeps making progress, and let it resynchronise on its next IDR. */
            bool idle = (xTaskGetTickCount() - s_net.last_consume) > pdMS_TO_TICKS(NET_CONSUMER_IDLE_MS);
            if (idle) {
                /* Nobody is draining the queue: skip this frame instead of
                 * evicting the ones already waiting for a session. */
                xSemaphoreGive(s_net.lock);
                return drain_frame(fd, au_len);
            }
            s_net.head = (uint8_t)((s_net.head + 1) % NET_SLOTS);
            s_net.count--;
            s_net.dropped_overflow++;
            ESP_LOGW(TAG, "queue full: drop oldest after %lums without a consume (depth %u)",
                     (unsigned long)(xTaskGetTickCount() - s_net.last_consume),
                     (unsigned)s_net.count);
            /* The queue length and the semaphore must stay in step. */
            xSemaphoreTake(s_net.queued, 0);
            room = true;
        }
        if (room) {
            uint8_t slot = (uint8_t)((s_net.head + s_net.count) % NET_SLOTS);
            xSemaphoreGive(s_net.lock);
            if (!recv_all(fd, s_net.slots[slot], au_len)) {
                return false;
            }
            xSemaphoreTake(s_net.lock, portMAX_DELAY);
            s_net.slot_len[slot] = au_len;
            s_net.slot_idr[slot] = (header[6] & NET_FLAG_IDR) != 0;
            s_net.count++;
            xSemaphoreGive(s_net.lock);
            xSemaphoreGive(s_net.queued);
            return true;
        }
        xSemaphoreGive(s_net.lock);
        vTaskDelay(pdMS_TO_TICKS(NET_PUSH_POLL_MS));
    }
}

static void capture_task(void *arg)
{
    (void)arg;

    for (;;) {
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(s_net.port),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listener < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int reuse = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            listen(listener, 1) != 0) {
            ESP_LOGE(TAG, "cannot listen on TCP %u", (unsigned)s_net.port);
            close(listener);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "waiting for a capture client on TCP %u", (unsigned)s_net.port);

        int client = accept(listener, NULL, NULL);
        close(listener);
        if (client < 0) {
            continue;
        }
        ESP_LOGI(TAG, "capture client connected");

        bool alive = handle_config(client);
        while (alive) {
            alive = handle_frame(client);
        }

        close(client);
        s_net.configured = false;
        s_source.width = 0;
        s_source.height = 0;
        /* Drop whatever the closed client left behind; the next one starts a
         * new quality epoch with its own parameter sets and IDR. */
        xSemaphoreTake(s_net.lock, portMAX_DELAY);
        s_net.head = 0;
        s_net.count = 0;
        xSemaphoreGive(s_net.lock);
        while (xSemaphoreTake(s_net.queued, 0) == pdTRUE) {
        }
        ESP_LOGW(TAG, "capture client disconnected (%u frame(s) dropped, %u before an IDR, "
                  "%u oversized)",
                 (unsigned)s_net.dropped_overflow, (unsigned)s_net.dropped,
                 (unsigned)s_net.oversized);
        s_net.dropped_overflow = 0;
        s_net.dropped = 0;
        s_net.oversized = 0;
    }
}

static const uint8_t *net_codec_record(void *user, size_t *len)
{
    (void)user;
    if (!s_net.configured) {
        return NULL;
    }
    /* A session starts here. Whatever is already queued was produced for a
     * previous session - or before the encoder's last IDR - and a P frame it
     * cannot reference leaves the receiver showing nothing at all. */
    xSemaphoreTake(s_net.lock, portMAX_DELAY);
    s_net.head = 0;
    s_net.count = 0;
    s_net.need_idr = true;
    xSemaphoreGive(s_net.lock);
    while (xSemaphoreTake(s_net.queued, 0) == pdTRUE) {
    }
    *len = s_net.avcc_len;
    return s_net.avcc;
}

static esp_err_t net_next_frame(void *user, const uint8_t **au, size_t *len, bool *idr)
{
    (void)user;

    /* Wait long enough for a slow client: ScreenCaptureKit only delivers frames
     * when the captured content changes, so a quiet window can leave gaps of
     * several hundred milliseconds. Timing out early would drop a slot and halve
     * the effective rate. */
    uint32_t wait_ms = 500;
    for (;;) {
        if (xSemaphoreTake(s_net.queued, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }

        xSemaphoreTake(s_net.lock, portMAX_DELAY);
        uint8_t slot = s_net.head;
        s_net.head = (uint8_t)((s_net.head + 1) % NET_SLOTS);
        s_net.count--;
        s_net.last_consume = xTaskGetTickCount();
        bool waiting_for_idr = s_net.need_idr && !s_net.slot_idr[slot];
        if (!waiting_for_idr) {
            s_net.need_idr = false;
        }
        if (waiting_for_idr) {
            s_net.dropped++;
        }
        xSemaphoreGive(s_net.lock);

        if (waiting_for_idr) {
            /* The stream is undecodable until the next IDR; the client is asked
             * for one every few seconds, so this gap stays short. */
            ESP_LOGI(TAG, "waiting for an IDR to start the stream");
            wait_ms = 2000;
            continue;
        }

        *au = s_net.slots[slot];
        *len = s_net.slot_len[slot];
        *idr = s_net.slot_idr[slot];
        return ESP_OK;
    }
}

bool media_net_init(uint16_t port)
{
    if (s_net.queued != NULL) {
        return true;
    }
    s_net.port = port;
    s_net.avcc = big_alloc(NET_AVCC_CAP);
    if (s_net.avcc == NULL) {
        ESP_LOGE(TAG, "no memory for the codec record");
        return false;
    }
    for (int i = 0; i < NET_SLOTS; i++) {
        s_net.slots[i] = big_alloc(NET_MAX_AU);
        if (s_net.slots[i] == NULL) {
            ESP_LOGE(TAG, "no memory for a capture buffer");
            return false;
        }
    }
    s_net.queued = xSemaphoreCreateCounting(NET_QUEUE_DEPTH, 0);
    s_net.lock = xSemaphoreCreateMutex();
    if (s_net.queued == NULL || s_net.lock == NULL) {
        return false;
    }
    s_source = (airplay_live_source_t){
        .name = "capture stream",
        .width = 0,
        .height = 0,
        /* No nominal interval: the client's frames are already paced by the
         * screen, and next_frame() blocks until the next one is queued. */
        .interval_ms = 0,
        .max_au_len = NET_MAX_AU,
        .codec_record = net_codec_record,
        .next_frame = net_next_frame,
        .user = NULL,
    };
    return true;
}

/* The listener needs lwIP, so it starts only after the network is up: a socket
 * call before esp_netif/lwIP are initialised faults on the ESP32-S3. */
bool media_net_start(void)
{
    if (s_net.queued == NULL) {
        return false;
    }
    if (s_net.started) {
        return true;
    }
    s_net.started = true;
    if (xTaskCreate(capture_task, "media_net", 8192, NULL, 4, NULL) != pdPASS) {
        s_net.started = false;
        return false;
    }
    return true;
}

bool media_net_ready(void)
{
    return s_net.configured;
}

const airplay_live_source_t *media_net_source(void)
{
    if (!s_net.configured) {
        return NULL;
    }
    return &s_source;
}
