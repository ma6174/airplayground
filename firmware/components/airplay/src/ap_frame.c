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
