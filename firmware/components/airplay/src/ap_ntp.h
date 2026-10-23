/*
 * NTP timing: the sender advertises a local UDP port in the control SETUP, and
 * the receiver probes it to map the frame timestamps onto its own clock.
 *
 * Frame timestamps are boot-relative (seconds.32-bit fraction) while the timing
 * replies carry the 1900 epoch, exactly as real senders do; the receiver
 * subtracts the epoch when it correlates the two.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct ap_session;

/* Binds the timing socket and reports the port advertised in SETUP. */
int ap_ntp_open(uint16_t *port_out);
void ap_ntp_close(int fd);

/* Spawns the responder task. Returns 0 when the task is running. */
int ap_ntp_start(struct ap_session *session);

/* Boot-relative fixed point, no epoch and no bias. */
uint64_t ap_ntp_boot_timestamp(void);

/* Boot-relative fixed point plus the 75 ms video playout lead. */
uint64_t ap_ntp_video_timestamp(void);

/* Frame timestamps. NTP mode stamps our own boot clock (timeline 0); PTP mode
 * follows the timeline the receiver advertised, anchored on the instant its
 * SETUP reply arrived. */
typedef struct {
    bool ptp;
    uint64_t timeline_id;
    uint64_t anchor;    /* NTP fixed point on the selected timeline */
    uint64_t anchor_us; /* boot microseconds at the anchor */
} ap_clock_t;

void ap_clock_use_ntp(ap_clock_t *clock);

/* have_receiver_time selects the receiver's own clock; without it (third-party
 * receivers omit the private headers) the timeline is anchored to our boot
 * clock instead, which is what a PTP peer with no shared time source does. */
void ap_clock_anchor_ptp(ap_clock_t *clock, uint64_t timeline_id, bool have_receiver_time,
                         uint64_t received_ms, uint64_t processing_ms);

uint64_t ap_clock_now(const ap_clock_t *clock, uint64_t *timeline_id);
