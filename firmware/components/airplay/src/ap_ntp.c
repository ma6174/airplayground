#include "ap_ntp.h"

#include <string.h>

#include "ap_internal.h"

static const char *TAG = "airplay";

/* Playback lead added to every frame timestamp, in microseconds. */
#define AP_VIDEO_TIMESTAMP_BIAS_US 75000

static uint64_t timestamp_from_us(uint64_t usec)
{
    uint64_t seconds = usec / 1000000u;
    uint64_t fraction = ((usec % 1000000u) << 32) / 1000000u;
    return (seconds << 32) | fraction;
}

uint64_t ap_ntp_boot_timestamp(void)
{
    return timestamp_from_us(ap_micros());
}

uint64_t ap_ntp_video_timestamp(void)
{
    return timestamp_from_us(ap_micros() + AP_VIDEO_TIMESTAMP_BIAS_US);
}

void ap_clock_use_ntp(ap_clock_t *clock)
{
    memset(clock, 0, sizeof(*clock));
}

void ap_clock_anchor_ptp(ap_clock_t *clock, uint64_t timeline_id, bool have_receiver_time,
                         uint64_t received_ms, uint64_t processing_ms)
{
    memset(clock, 0, sizeof(*clock));
    clock->ptp = true;
    clock->timeline_id = timeline_id;
    clock->anchor_us = ap_micros();
    if (have_receiver_time) {
        /* The receiver reports when the request reached it and how long it took;
         * both are milliseconds on its own timeline. */
        clock->anchor = timestamp_from_us((received_ms + processing_ms) * 1000u);
    } else {
        clock->anchor = timestamp_from_us(clock->anchor_us);
    }
}

uint64_t ap_clock_now(const ap_clock_t *clock, uint64_t *timeline_id)
{
    if (!clock->ptp) {
        *timeline_id = 0;
        return timestamp_from_us(ap_micros() + AP_VIDEO_TIMESTAMP_BIAS_US);
    }
    *timeline_id = clock->timeline_id;
    return clock->anchor + timestamp_from_us(ap_micros() - clock->anchor_us + AP_VIDEO_TIMESTAMP_BIAS_US);
}

int ap_ntp_open(uint16_t *port_out)
{
    return ap_socket_udp_open(port_out);
}

void ap_ntp_close(int fd)
{
    ap_socket_close(fd);
}

static void put_be64(uint8_t *out, uint64_t value)
{
    for (int i = 0; i < 8; i++) {
        out[7 - i] = (uint8_t)(value >> (8 * i));
    }
}

static void ntp_task(void *argument)
{
    ap_session_t *session = (ap_session_t *)argument;
    uint8_t request[64];
    uint8_t reply[32];

    while (!session->stop) {
        if (ap_socket_wait(session->timing_fd, false, 500) != 1) {
            continue;
        }

        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int received = recvfrom(session->timing_fd, request, sizeof(request), 0,
                                (struct sockaddr *)&peer, &peer_len);
        if (received < (int)sizeof(reply)) {
            continue;
        }
        /* 0xd2 is a request, 0xd3 a reply. Answering a reply would make the two
         * peers ping-pong forever. */
        if (request[0] != 0x80 || request[1] != 0xd2) {
            continue;
        }

        memcpy(reply, request, sizeof(reply));
        reply[0] = 0x80;
        reply[1] = 0xd3;
        /* Reference = the receiver's transmit timestamp. */
        memcpy(reply + 8, request + 24, 8);
        /* The reply carries the same clock the video timestamps use: the
         * receiver derives its playback offset from this difference, so a
         * reply on a different epoch (1900 instead of boot) moves every frame
         * by decades and it silently drops them all. */
        uint64_t now = ap_ntp_boot_timestamp();
        put_be64(reply + 16, now);
        put_be64(reply + 24, now);

        if (sendto(session->timing_fd, reply, sizeof(reply), 0, (struct sockaddr *)&peer, peer_len) < 0) {
            AP_LOGW("NTP reply failed");
            continue;
        }
        AP_LOGI("NTP timing reply sent to %s", inet_ntoa(peer.sin_addr));
    }

    vTaskDelete(NULL);
}

int ap_ntp_start(ap_session_t *session)
{
    if (xTaskCreate(ntp_task, "ap_ntp", 3072, session, 4, &session->ntp_task) != pdPASS) {
        AP_LOGE("could not start the NTP responder task");
        return -1;
    }
    return 0;
}
