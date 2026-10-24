#include "media_avcc.h"

#include <string.h>

static bool find_start_code(const uint8_t *data, size_t len, size_t from, size_t *code_at,
                            size_t *code_len)
{
    for (size_t i = from; i + 2 < len; i++) {
        if (data[i] != 0 || data[i + 1] != 0) {
            continue;
        }
        if (data[i + 2] == 1) {
            *code_at = i;
            *code_len = 3;
            return true;
        }
        if (i + 3 < len && data[i + 2] == 0 && data[i + 3] == 1) {
            *code_at = i;
            *code_len = 4;
            return true;
        }
    }
    return false;
}

void media_nal_begin(media_nal_iter_t *it, const uint8_t *data, size_t len)
{
    it->data = data;
    it->len = len;
    it->offset = 0;
    it->nal_len = 0;
    it->type = -1;
}

bool media_nal_next(media_nal_iter_t *it)
{
    size_t search_from = (it->nal_len > 0) ? it->offset + it->nal_len : 0;
    size_t code_at = 0;
    size_t code_len = 0;
    if (!find_start_code(it->data, it->len, search_from, &code_at, &code_len)) {
        return false;
    }

    /* The scanner matches a four-byte start code as a whole, so the payload
     * starts cleanly after it and ends right at the next code. */
    size_t payload = code_at + code_len;
    size_t end = it->len;
    size_t next_at = 0;
    size_t next_len = 0;
    if (find_start_code(it->data, it->len, payload, &next_at, &next_len)) {
        end = next_at;
    }
    if (end <= payload) {
        return false;
    }
    it->offset = payload;
    it->nal_len = end - payload;
    it->type = it->data[payload] & 0x1f;
    return true;
}

size_t media_avcc_record(const uint8_t *sps, size_t sps_len, const uint8_t *pps, size_t pps_len,
                         uint8_t *out, size_t out_cap)
{
    size_t need = 6 + 2 + sps_len + 1 + 2 + pps_len + 4;
    if (out == NULL || out_cap < need || sps_len < 4 || pps_len < 1) {
        return 0;
    }

    size_t o = 0;
    out[o++] = 0x01;        /* configurationVersion */
    out[o++] = sps[1];      /* AVCProfileIndication */
    out[o++] = sps[2];      /* profile_compatibility */
    out[o++] = sps[3];      /* AVCLevelIndication */
    out[o++] = 0xff;        /* 0b111111: lengthSizeMinusOne = 3 (4-byte lengths) */
    out[o++] = 0xe1;        /* 0b111: one SPS, reserved bits */
    out[o++] = (uint8_t)(sps_len >> 8);
    out[o++] = (uint8_t)sps_len;
    memcpy(out + o, sps, sps_len);
    o += sps_len;
    out[o++] = 0x01;        /* one PPS */
    out[o++] = (uint8_t)(pps_len >> 8);
    out[o++] = (uint8_t)pps_len;
    memcpy(out + o, pps, pps_len);
    o += pps_len;
    out[o++] = 0x02;        /* the four trailing bytes genuine senders append */
    out[o++] = 0x00;
    out[o++] = 0x00;
    out[o++] = 0x00;
    return o;
}

bool media_avcc_wrap(const uint8_t *annexb, size_t annexb_len, uint8_t *out, size_t out_cap,
                     size_t *out_len, bool *idr)
{
    size_t o = 0;
    bool found_idr = false;

    media_nal_iter_t it;
    media_nal_begin(&it, annexb, annexb_len);
    while (media_nal_next(&it)) {
        if (o + 4 + it.nal_len > out_cap) {
            return false;
        }
        out[o++] = (uint8_t)(it.nal_len >> 24);
        out[o++] = (uint8_t)(it.nal_len >> 16);
        out[o++] = (uint8_t)(it.nal_len >> 8);
        out[o++] = (uint8_t)it.nal_len;
        memcpy(out + o, annexb + it.offset, it.nal_len);
        o += it.nal_len;
        if (it.type == 5) {
            found_idr = true;
        }
    }
    if (o == 0) {
        return false;
    }
    *out_len = o;
    *idr = found_idr;
    return true;
}
