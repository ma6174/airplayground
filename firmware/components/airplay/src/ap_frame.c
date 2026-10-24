#include "ap_frame.h"

#include <string.h>

static void put_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void put_u64_le(uint8_t *out, uint64_t value)
{
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(value >> (8 * i));
    }
}

static void put_float_le(uint8_t *out, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put_u32_le(out, bits);
}

void ap_frame_build_codec(uint8_t header[AP_FRAME_HEADER_LEN], size_t payload_len, uint64_t timestamp,
                          uint16_t width, uint16_t height)
{
    memset(header, 0, AP_FRAME_HEADER_LEN);
    put_u32_le(header, (uint32_t)payload_len);
    header[4] = 0x01; /* decoder configuration */
    header[5] = 0x00;
    header[6] = 0x16; /* H.264 avcC */
    header[7] = 0x01;
    put_u64_le(header + 8, timestamp);

    /* Encoded size, then the source and destination rectangles. A sender with
     * no separate placement metadata describes the full raster twice. */
    put_float_le(header + 16, (float)width);
    put_float_le(header + 20, (float)height);
    put_float_le(header + 40, (float)width);
    put_float_le(header + 44, (float)height);
    put_float_le(header + 56, (float)width);
    put_float_le(header + 60, (float)height);
}

void ap_frame_build_video(uint8_t header[AP_FRAME_HEADER_LEN], size_t payload_len_with_tag, bool idr,
                          uint64_t timestamp, uint64_t timeline_id)
{
    memset(header, 0, AP_FRAME_HEADER_LEN);
    put_u32_le(header, (uint32_t)payload_len_with_tag);
    header[4] = 0x00; /* encrypted video data */
    header[5] = idr ? 0x10 : 0x00;
    put_u64_le(header + 8, timestamp);
    put_u64_le(header + 40, timeline_id);
}

void ap_frame_build_heartbeat(uint8_t header[AP_FRAME_HEADER_LEN])
{
    memset(header, 0, AP_FRAME_HEADER_LEN);
    header[4] = 0x02;
    header[6] = 0x1e;
}

void ap_frame_build_legacy(uint8_t header[AP_FRAME_HEADER_LEN], size_t payload_len,
                           uint8_t record_type, uint64_t timestamp, uint16_t width,
                           uint16_t height, bool keyframe)
{
    memset(header, 0, AP_FRAME_HEADER_LEN);
    put_u32_le(header, (uint32_t)payload_len);
    header[4] = record_type; /* 0x00 video, 0x01 codec config */
    if (record_type == 0x01) {
        header[6] = 0x16;
        header[7] = 0x01;
    } else if (keyframe) {
        header[5] = 0x10;
    }
    if (record_type == 0x00) {
        /* Boot-relative NTP 32.32, little-endian, no epoch (RPiPlay
         * lib/raop_rtp_mirror.c:294-300). The IDR hint byte is masked off by
         * the receiver but iOS sets it, so keep it. */
        put_u64_le(header + 8, timestamp);
    } else {
        put_float_le(header + 16, (float)width);
        put_float_le(header + 20, (float)height);
        put_float_le(header + 40, (float)width);
        put_float_le(header + 44, (float)height);
        put_float_le(header + 56, (float)width);
        put_float_le(header + 60, (float)height);
    }
}

static size_t copy_nalu(uint8_t *out, size_t cap, size_t offset, const uint8_t *nalu,
                        size_t nalu_len)
{
    if (offset + 4 + nalu_len > cap) {
        return 0;
    }
    out[offset + 0] = (uint8_t)(nalu_len >> 24);
    out[offset + 1] = (uint8_t)(nalu_len >> 16);
    out[offset + 2] = (uint8_t)(nalu_len >> 8);
    out[offset + 3] = (uint8_t)nalu_len;
    memcpy(out + offset + 4, nalu, nalu_len);
    return offset + 4 + nalu_len;
}

size_t ap_frame_add_parameter_sets(uint8_t *out, size_t cap, const uint8_t *avcc,
                                   size_t avcc_len, const uint8_t *au, size_t au_len)
{
    if (avcc_len < 7 || avcc[0] != 1) {
        return 0;
    }
    size_t offset = 0;
    size_t cursor = 6;
    size_t sps_count = avcc[5] & 0x1f;
    for (size_t i = 0; i < sps_count; i++) {
        if (cursor + 2 > avcc_len) {
            return 0;
        }
        size_t sps_len = ((size_t)avcc[cursor] << 8) | avcc[cursor + 1];
        cursor += 2;
        if (cursor + sps_len > avcc_len) {
            return 0;
        }
        offset = copy_nalu(out, cap, offset, avcc + cursor, sps_len);
        if (offset == 0) {
            return 0;
        }
        cursor += sps_len;
    }
    if (cursor >= avcc_len) {
        return 0;
    }
    size_t pps_count = avcc[cursor++];
    for (size_t i = 0; i < pps_count; i++) {
        if (cursor + 2 > avcc_len) {
            return 0;
        }
        size_t pps_len = ((size_t)avcc[cursor] << 8) | avcc[cursor + 1];
        cursor += 2;
        if (cursor + pps_len > avcc_len) {
            return 0;
        }
        offset = copy_nalu(out, cap, offset, avcc + cursor, pps_len);
        if (offset == 0) {
            return 0;
        }
        cursor += pps_len;
    }
    if (offset + au_len > cap) {
        return 0;
    }
    memcpy(out + offset, au, au_len);
    return offset + au_len;
}

bool ap_avcc_has_idr(const uint8_t *avcc, size_t len)
{
    size_t offset = 0;
    while (offset + 4 <= len) {
        size_t nal_len = ((size_t)avcc[offset] << 24) | ((size_t)avcc[offset + 1] << 16) |
                         ((size_t)avcc[offset + 2] << 8) | (size_t)avcc[offset + 3];
        offset += 4;
        if (nal_len == 0 || nal_len > len - offset) {
            return false;
        }
        if ((avcc[offset] & 0x1f) == 5) {
            return true;
        }
        offset += nal_len;
    }
    return false;
}
