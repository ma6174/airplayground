#include "media_net.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include "media_avcc.h"

static const char *TAG = "media_net";

/* Wire format, all fields big-endian:
 *
 *   CONFIG  magic 'APVN' | type 1 | version 1 | reserved 2 | width 2 |
 *           height 2 | fps 2 | sps_len 2 | pps_len 2 | sps | pps
 *   FRAME   magic 'APVN' | type 2 | version 1 | flags 1 | reserved 2 |
 *           au_len 4 | pts_ms 4 | AVCC access unit
 *   AUDIO   magic 'APVN' | type 3 | version 1 | flags 0 | reserved 2 |
 *           frame_len 4 | pts_ms 4 | one encoded audio frame (ALAC, spf samples)
 *
 * flags bit 0 marks an IDR access unit. Every access unit carries its own
 * boundaries, so the receiving side never parses H.264 to split TCP reads. */
#define NET_MAGIC 0x4150564eu
#define NET_VERSION 1
#define NET_TYPE_CONFIG 1
#define NET_TYPE_FRAME 2
#define NET_TYPE_AUDIO 3
#define NET_FLAG_IDR 0x01u
#define NET_MAX_AU (512u * 1024u)
/* A frame pointer that is absurdly large means the stream lost sync, not that
 * the encoder went wild; only then is dropping the connection the right move. */
#define NET_MAX_DRAIN (4u * 1024u * 1024u)
#define NET_MAX_PARAM 1024u
/* Audio rides the same connection but in its own ring: one frame per audio
 * packet the session sends, 8 ms of sound each, and the session drains exactly
 * one per packet. Losing an audio frame is a click, so the ring only turns into
 * "drop the oldest" once the consumer has been idle long enough to mean nobody
 * is listening. */
/* Audio shares the connection with video, so one 28 KB picture holds the audio
 * writes back for tens of milliseconds, and a keyframe can stall the whole
 * connection for a fifth of a second. The ring covers half a second of sound, so
 * the client can stay well ahead and neither starve nor overflow. */
#define NET_AUDIO_DEPTH 64
/* Well over the 1415 bytes of one 8 ms ALAC frame: a client that packs more
 * samples into a frame still fits, while a stream that lost sync cannot aim a
 * read at an arbitrary size. */
#define NET_AUDIO_MAX (8u * 1024u)
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
/* A pull target that is not answering is retried on this cadence: fast enough
 * that a laptop returning from sleep restores the picture promptly, slow enough
 * to keep the radio and the log quiet. The listener stays up meanwhile, so a
 * capture client that dials in still wins. */
#define NET_PULL_RETRY_MS 3000

/* The target lives in NVS so a reboot resumes pulling without anyone asking
 * again - the whole point of pulling is that the device repairs the stream on
 * its own. */
#define PULL_NVS_NAMESPACE "media_net"
#define PULL_NVS_HOST "pull_host"
#define PULL_NVS_PORT "pull_port"

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
    /* Audio ring: head plus count, oldest first. */
    uint8_t *audio_slots[NET_AUDIO_DEPTH];
    size_t audio_len[NET_AUDIO_DEPTH];
    uint8_t audio_head;
    uint8_t audio_count;
    uint32_t audio_dropped;
    uint32_t audio_received;
    /* The frame is copied out of the ring for the session (see
     * media_net_audio), so the slot is free the moment it is popped. */
    uint8_t *audio_copy;
    /* Connection the capture client reads the arm byte from, and whether a
     * session has started using this ring. */
    int audio_client;
    bool audio_armed;
    /* Pull target: while set, both tasks dial "host:port" instead of waiting
     * for a client. */
    char pull_host[64];
    uint16_t pull_port;
    volatile bool pull_on;
    /* Sockets of a running connection, so stopping a pull can break a read that
     * is already in flight. */
    volatile int video_fd;
    volatile int audio_fd;
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

static int listener_open(uint16_t port)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listener < 0) {
        return -1;
    }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listener, 1) != 0) {
        ESP_LOGE(TAG, "cannot listen on TCP %u", (unsigned)port);
        close(listener);
        return -1;
    }
    ESP_LOGI(TAG, "waiting for a capture client on TCP %u", (unsigned)port);
    return listener;
}

static int pull_connect(uint16_t port)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    if (inet_pton(AF_INET, s_net.pull_host, &addr.sin_addr) != 1) {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* The next connection on this port, however it arrives: this side dials the pull
 * target when one is set, and keeps listening meanwhile. The two are
 * alternatives rather than modes - a capture client that dials in still gets the
 * screen even while a pull target is configured but unreachable. */
static int wait_for_peer(uint16_t port)
{
    int listener = -1;
    TickType_t retry_at = 0;
    bool complained = false;
    for (;;) {
        if (s_net.pull_on && xTaskGetTickCount() >= retry_at) {
            int fd = pull_connect(port);
            if (fd >= 0) {
                if (listener >= 0) {
                    /* The pull owns the stream now; a second producer would fight
                     * it for the queue. */
                    close(listener);
                }
                ESP_LOGI(TAG, "pulling from %s on TCP %u", s_net.pull_host, (unsigned)port);
                return fd;
            }
            retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(NET_PULL_RETRY_MS);
            if (!complained) {
                ESP_LOGW(TAG, "nothing to pull from %s on TCP %u, retrying every %d ms",
                         s_net.pull_host, (unsigned)port, NET_PULL_RETRY_MS);
                complained = true;
            }
        }
        if (!s_net.pull_on) {
            complained = false;
        }
        if (listener < 0) {
            listener = listener_open(port);
            if (listener < 0) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }
        /* Short waits so a pull target set from the bench API takes effect
         * within a fifth of a second. */
        if (!recv_ready(listener, 200)) {
            continue;
        }
        int client = accept(listener, NULL, NULL);
        close(listener);
        return client;
    }
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

/* One encoded audio frame: queue it for the session, which sends exactly one per
 * audio packet. */
static bool handle_audio(int fd, const uint8_t *header)
{
    uint32_t len = read_be32(header + 8);
    if (len == 0 || len > NET_AUDIO_MAX) {
        ESP_LOGW(TAG, "audio frame of %u bytes is out of range", (unsigned)len);
        return len == 0 || drain_frame(fd, len);
    }
    uint8_t slot = (uint8_t)((s_net.audio_head + s_net.audio_count) % NET_AUDIO_DEPTH);
    if (s_net.audio_count == NET_AUDIO_DEPTH) {
        /* Full: keep the newest audio and give up the oldest frame. */
        s_net.audio_head = (uint8_t)((s_net.audio_head + 1) % NET_AUDIO_DEPTH);
        s_net.audio_count--;
        s_net.audio_dropped++;
        if (s_net.audio_dropped % 50 == 1) {
            ESP_LOGW(TAG, "audio queue full: dropped %u frame(s)", (unsigned)s_net.audio_dropped);
        }
        slot = (uint8_t)((s_net.audio_head + s_net.audio_count) % NET_AUDIO_DEPTH);
    }
    if (!recv_all(fd, s_net.audio_slots[slot], len)) {
        return false;
    }
    s_net.audio_received++;
    xSemaphoreTake(s_net.lock, portMAX_DELAY);
    s_net.audio_len[slot] = len;
    s_net.audio_count++;
    xSemaphoreGive(s_net.lock);
    return true;
}

static bool handle_frame(int fd)
{
    uint8_t header[NET_HEADER_MAX];
    if (!recv_all(fd, header, 16) || read_be32(header) != NET_MAGIC ||
        header[5] != NET_VERSION) {
        ESP_LOGW(TAG, "malformed frame packet");
        return false;
    }
    if (header[4] == NET_TYPE_AUDIO) {
        return handle_audio(fd, header);
    }
    if (header[4] != NET_TYPE_FRAME) {
        ESP_LOGW(TAG, "unknown packet type %u", (unsigned)header[4]);
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
        int client = wait_for_peer(s_net.port);
        if (client < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        s_net.video_fd = client;
        ESP_LOGI(TAG, "capture client connected");

        bool alive = handle_config(client);
        while (alive) {
            alive = handle_frame(client);
        }

        close(client);
        s_net.video_fd = -1;
        s_net.configured = false;
        s_source.width = 0;
        s_source.height = 0;
        /* Drop whatever the closed client left behind; the next one starts a
         * new quality epoch with its own parameter sets and IDR. */
        xSemaphoreTake(s_net.lock, portMAX_DELAY);
        s_net.head = 0;
        s_net.count = 0;
        s_net.audio_head = 0;
        s_net.audio_count = 0;
        xSemaphoreGive(s_net.lock);
        while (xSemaphoreTake(s_net.queued, 0) == pdTRUE) {
        }
        ESP_LOGW(TAG, "capture client disconnected (%u frame(s) dropped, %u before an IDR, "
                  "%u oversized, %u audio)",
                 (unsigned)s_net.dropped_overflow, (unsigned)s_net.dropped,
                 (unsigned)s_net.oversized, (unsigned)s_net.audio_dropped);
        s_net.dropped_overflow = 0;
        s_net.dropped = 0;
        s_net.oversized = 0;
        s_net.audio_dropped = 0;
    }
}

/* The capture client paces ALAC frames from its own clock, one frame per
 * 8 ms, and the ring it fills is emptied the moment a session starts. Its lead
 * over the device therefore has to be rebuilt then, or the device starts every
 * session with an empty ring and starves on a third of its slots. One byte on
 * the audio connection says "the channel is live now": clients that ignore it
 * lose nothing but the lead.
 *
 * Caller holds the lock. */
static void audio_arm_locked(void)
{
    if (!s_net.audio_armed || s_net.audio_client < 0) {
        return;
    }
    uint8_t byte = 1;
    if (send(s_net.audio_client, &byte, 1, MSG_DONTWAIT) != 1) {
        ESP_LOGW(TAG, "cannot send the audio arm byte");
    }
}

/* Audio gets its own connection: on a shared one, a 28 KB keyframe holds the
 * audio writes back for a fifth of a second, which is long enough to starve the
 * receiver and be heard as a click. */
void audio_capture_task(void *arg)
{
    (void)arg;
    const uint16_t port = (uint16_t)(s_net.port + 1);
    for (;;) {
        int client = wait_for_peer(port);
        if (client < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        s_net.audio_fd = client;
        int window = 64 * 1024;
        setsockopt(client, SOL_SOCKET, SO_RCVBUF, &window, sizeof(window));
        /* A new audio client means a new run: the counters describe it alone. */
        s_net.audio_received = 0;
        xSemaphoreTake(s_net.lock, portMAX_DELAY);
        s_net.audio_client = client;
        audio_arm_locked();
        xSemaphoreGive(s_net.lock);
        ESP_LOGI(TAG, "audio client connected");
        for (;;) {
            uint8_t header[NET_HEADER_MAX];
            if (!recv_all(client, header, 16) || read_be32(header) != NET_MAGIC ||
                header[4] != NET_TYPE_AUDIO || header[5] != NET_VERSION) {
                ESP_LOGW(TAG, "malformed audio packet");
                break;
            }
            if (!handle_audio(client, header)) {
                break;
            }
        }
        close(client);
        s_net.audio_fd = -1;
        ESP_LOGW(TAG, "audio client disconnected (%u frame(s) dropped)",
                 (unsigned)s_net.audio_dropped);
        s_net.audio_dropped = 0;
        xSemaphoreTake(s_net.lock, portMAX_DELAY);
        s_net.audio_client = -1;
        s_net.audio_head = 0;
        s_net.audio_count = 0;
        xSemaphoreGive(s_net.lock);
    }
}

static const uint8_t *net_codec_record(void *user, size_t *len)
{
    (void)user;
    if (!s_net.configured) {
        return NULL;
    }
    /* A session starts here. Audio queued before it belongs to nobody's
     * timeline, and letting that backlog drain would clip the first seconds.
     * The client that paced those frames believes the ring still holds the lead
     * it built up, so the arm byte below is what tells it to build that lead
     * again - an empty ring absorbs no jitter and starves the receiver. */
    xSemaphoreTake(s_net.lock, portMAX_DELAY);
    s_net.audio_head = 0;
    s_net.audio_count = 0;
    s_net.audio_armed = true;
    audio_arm_locked();
    xSemaphoreGive(s_net.lock);
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

static void pull_store(void)
{
    nvs_handle_t handle;
    if (nvs_open(PULL_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "cannot persist the pull target");
        return;
    }
    if (s_net.pull_host[0] == '\0') {
        nvs_erase_key(handle, PULL_NVS_HOST);
        nvs_erase_key(handle, PULL_NVS_PORT);
    } else {
        nvs_set_str(handle, PULL_NVS_HOST, s_net.pull_host);
        nvs_set_u16(handle, PULL_NVS_PORT, s_net.pull_port);
    }
    nvs_commit(handle);
    nvs_close(handle);
}

static void pull_load(void)
{
    nvs_handle_t handle;
    if (nvs_open(PULL_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_net.pull_host);
    uint16_t port = 0;
    if (nvs_get_str(handle, PULL_NVS_HOST, s_net.pull_host, &len) == ESP_OK &&
        nvs_get_u16(handle, PULL_NVS_PORT, &port) == ESP_OK && port != 0) {
        s_net.pull_port = port;
        s_net.pull_on = true;
        ESP_LOGI(TAG, "pull target from NVS: %s:%u", s_net.pull_host, (unsigned)port);
    }
    nvs_close(handle);
}

bool media_net_pull(const char *host, uint16_t port)
{
    if (host == NULL || host[0] == '\0' || port == 0) {
        media_net_pull_stop();
        return true;
    }
    if (strlen(host) >= sizeof(s_net.pull_host)) {
        return false;
    }
    /* A different target means the connection in flight points at the old one. */
    if (strcmp(s_net.pull_host, host) != 0 || s_net.pull_port != port) {
        media_net_pull_stop();
    }
    strncpy(s_net.pull_host, host, sizeof(s_net.pull_host) - 1);
    s_net.pull_host[sizeof(s_net.pull_host) - 1] = '\0';
    s_net.pull_port = port;
    s_net.pull_on = true;
    pull_store();
    ESP_LOGI(TAG, "pulling the capture stream from %s:%u", host, (unsigned)port);
    return true;
}

void media_net_pull_stop(void)
{
    if (!s_net.pull_on) {
        return;
    }
    s_net.pull_on = false;
    s_net.pull_host[0] = '\0';
    s_net.pull_port = 0;
    pull_store();
    /* A connection that is already pulling sits in recv(); shutting it down is
     * what gets the task back to waiting for a client. */
    int video = s_net.video_fd;
    int audio = s_net.audio_fd;
    if (video >= 0) {
        shutdown(video, SHUT_RDWR);
    }
    if (audio >= 0) {
        shutdown(audio, SHUT_RDWR);
    }
    ESP_LOGI(TAG, "pull stopped; waiting for a capture client again");
}

void media_net_drop_client(void)
{
    int video = s_net.video_fd;
    int audio = s_net.audio_fd;
    if (video >= 0) {
        shutdown(video, SHUT_RDWR);
    }
    if (audio >= 0) {
        shutdown(audio, SHUT_RDWR);
    }
    ESP_LOGI(TAG, "capture connection dropped for another source");
}

const char *media_net_pull_target(void)
{
    static char text[80];
    text[0] = '\0';
    if (s_net.pull_on) {
        snprintf(text, sizeof(text), "%s:%u", s_net.pull_host, (unsigned)s_net.pull_port);
    }
    return text;
}

bool media_net_init(uint16_t port)
{
    if (s_net.queued != NULL) {
        return true;
    }
    s_net.port = port;
    s_net.video_fd = -1;
    s_net.audio_fd = -1;
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
    for (int i = 0; i < NET_AUDIO_DEPTH; i++) {
        s_net.audio_slots[i] = big_alloc(NET_AUDIO_MAX);
        if (s_net.audio_slots[i] == NULL) {
            ESP_LOGE(TAG, "no memory for an audio buffer");
            return false;
        }
    }
    s_net.audio_copy = big_alloc(NET_AUDIO_MAX);
    if (s_net.audio_copy == NULL) {
        ESP_LOGE(TAG, "no memory for the audio handoff buffer");
        return false;
    }
    s_net.audio_client = -1;
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
    pull_load();
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
    /* The audio reader outranks everything else here: while the session task is
     * busy pushing a 30 KB keyframe to the receiver, a delayed read backs up into
     * the client's socket and the sound starves. */
    if (xTaskCreate(audio_capture_task, "media_net_audio", 4096, NULL, 7, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no audio listener; audio has to share the video connection");
    }
    return true;
}

bool media_net_ready(void)
{
    return s_net.configured;
}

const uint8_t *media_net_audio(size_t *len)
{
    if (len != NULL) {
        *len = 0;
    }
    if (xSemaphoreTake(s_net.lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return NULL;
    }
    const uint8_t *frame = NULL;
    if (s_net.audio_count != 0) {
        size_t taken = s_net.audio_len[s_net.audio_head];
        /* Popping a frame hands its slot back to the producer, which fills it as
         * soon as the next frame arrives - usually while the caller is still
         * encrypting what it holds. Copy it out here, under the lock, and a
         * half-overwritten ALAC frame can never reach the speaker. */
        memcpy(s_net.audio_copy, s_net.audio_slots[s_net.audio_head], taken);
        frame = s_net.audio_copy;
        if (len != NULL) {
            *len = taken;
        }
        s_net.audio_head = (uint8_t)((s_net.audio_head + 1) % NET_AUDIO_DEPTH);
        s_net.audio_count--;
    }
    xSemaphoreGive(s_net.lock);
    return frame;
}

const airplay_live_source_t *media_net_source(void)
{
    if (!s_net.configured) {
        return NULL;
    }
    return &s_source;
}

uint32_t media_net_audio_received(void)
{
    return s_net.audio_received;
}
