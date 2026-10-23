/*
 * The 128-byte header that prefixes every packet on the video data channel,
 * plus the small amount of AVCC introspection the sender needs.
 *
 * Layout (all multi-byte fields little-endian, offsets from the spec):
 *   [0:4]   payload length, including the 16-byte Poly1305 tag when encrypted
 *   [4]     packet type: 0x00 video, 0x01 codec configuration, 0x02 heartbeat
 *   [5]     0x10 for an IDR access unit
 *   [6]     codec packet option: 0x16 H.264 avcC, 0x1e heartbeat
 *   [7]     0x01 for a codec packet
 *   [8:16]  boot-relative NTP timestamp
 *   [16:24] encoded width / height as float32 (codec packet)
 *   [40:48] timeline ID (video packet); target rect width/height (codec)
 *   [56:64] second rect width / height (codec packet)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AP_FRAME_HEADER_LEN 128
#define AP_FRAME_TAG_LEN 16

void ap_frame_build_codec(uint8_t header[AP_FRAME_HEADER_LEN], size_t payload_len, uint64_t timestamp,
                          uint16_t width, uint16_t height);

/* payload_len_with_tag is the plaintext length plus AP_FRAME_TAG_LEN. */
void ap_frame_build_video(uint8_t header[AP_FRAME_HEADER_LEN], size_t payload_len_with_tag, bool idr,
                          uint64_t timestamp, uint64_t timeline_id);

void ap_frame_build_heartbeat(uint8_t header[AP_FRAME_HEADER_LEN]);

/* Legacy (AirTunes) header: payload length and record type in [4], boot NTP
 * timestamp at [8:16] for video records, source/display float pairs at
 * [16:32] and [56:64] for the codec record. No timeline, no Poly1305 tag. */
void ap_frame_build_legacy(uint8_t header[AP_FRAME_HEADER_LEN], size_t payload_len,
                           uint8_t record_type, uint64_t timestamp, uint16_t width,
                           uint16_t height);

/* True when the AVCC access unit contains an IDR slice (NAL type 5). The
 * still-frame source does not label its frames, and the receiver needs the
 * keyframe flag to reset its decoder. */
bool ap_avcc_has_idr(const uint8_t *avcc, size_t len);
