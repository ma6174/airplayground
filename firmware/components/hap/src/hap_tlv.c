/*
 * TLV8: the record format every HAP pairing message is built from.
 * Values longer than 255 bytes are emitted as consecutive items with the same
 * tag, and a decoder has to join them back together.
 */
#include "hap_internal.h"

#include <string.h>

void hap_tlv_out_init(hap_tlv_out_t *out, uint8_t *buf, size_t cap)
{
    out->buf = buf;
    out->cap = cap;
    out->len = 0;
    out->overflow = false;
}

int hap_tlv_put(hap_tlv_out_t *out, uint8_t tag, const uint8_t *value, size_t value_len)
{
    if (value == NULL && value_len != 0) {
        return -1;
    }
    if (value_len == 0) {
        if (out->len + 2 > out->cap) {
            out->overflow = true;
            HAP_LOGE("tlv8: no room for tag 0x%02x", tag);
            return -1;
        }
        out->buf[out->len++] = tag;
        out->buf[out->len++] = 0;
        return 0;
    }

    while (value_len > 0) {
        size_t chunk = value_len > 255 ? 255 : value_len;
        if (out->len + 2 + chunk > out->cap) {
            out->overflow = true;
            HAP_LOGE("tlv8: message overflow while writing tag 0x%02x", tag);
            return -1;
        }
        out->buf[out->len++] = tag;
        out->buf[out->len++] = (uint8_t)chunk;
        memcpy(out->buf + out->len, value, chunk);
        out->len += chunk;
        value += chunk;
        value_len -= chunk;
    }
    return 0;
}

int hap_tlv_put_u8(hap_tlv_out_t *out, uint8_t tag, uint8_t value)
{
    return hap_tlv_put(out, tag, &value, 1);
}

int hap_tlv_get(const uint8_t *message, size_t message_len, uint8_t tag, uint8_t *value, size_t cap,
                size_t *value_len)
{
    size_t pos = 0;
    size_t used = 0;

    if (value_len == NULL) {
        return -1;
    }
    *value_len = 0;

    while (pos < message_len) {
        uint8_t item_tag;
        size_t item_len;

        if (message_len - pos < 2) {
            HAP_LOGE("tlv8: truncated item header");
            return -1;
        }
        item_tag = message[pos];
        item_len = message[pos + 1];
        pos += 2;
        if (item_len > message_len - pos) {
            HAP_LOGE("tlv8: truncated item body for tag 0x%02x", item_tag);
            return -1;
        }
        if (item_tag == tag) {
            if (item_len > cap - used) {
                HAP_LOGE("tlv8: tag 0x%02x needs %u bytes, cap is %u", tag,
                         (unsigned)(used + item_len), (unsigned)cap);
                return -1;
            }
            if (item_len != 0) {
                if (value == NULL) {
                    return -1;
                }
                memcpy(value + used, message + pos, item_len);
            }
            used += item_len;
        }
        pos += item_len;
    }

    *value_len = used;
    return 0;
}

int hap_tlv_error(const uint8_t *message, size_t message_len)
{
    uint8_t code = 0;
    size_t len = 0;

    if (hap_tlv_get(message, message_len, HAP_TLV_ERROR, &code, sizeof code, &len) != 0) {
        return -1;
    }
    if (len == 0) {
        return -1;
    }
    return code;
}
