#include "ap_data.h"

#include <string.h>

#include "ap_frame.h"
#include "ap_platform.h"

static const char *TAG = "airplay";

int ap_data_open(ap_data_channel_t *channel, const char *host, uint16_t port,
                 const uint8_t key[HAP_KEY_LEN])
{
    memset(channel, 0, sizeof(*channel));
    channel->fd = ap_socket_connect(host, port, AP_DATA_CONNECT_TIMEOUT_MS);
    if (channel->fd < 0) {
        return -1;
    }
    ap_socket_set_nodelay(channel->fd);
    channel->encrypted = key != NULL;
    if (key != NULL) {
        memcpy(channel->key, key, HAP_KEY_LEN);
    }
    return 0;
}

void ap_data_close(ap_data_channel_t *channel)
{
    if (channel->fd >= 0) {
        ap_socket_close(channel->fd);
        channel->fd = -1;
    }
}

int ap_data_send_codec(ap_data_channel_t *channel, const void *avcc, size_t avcc_len, uint16_t width,
                       uint16_t height, uint64_t timestamp)
{
    uint8_t header[AP_FRAME_HEADER_LEN];
    ap_frame_build_codec(header, avcc_len, timestamp, width, height);

    if (ap_socket_write_all(channel->fd, header, sizeof(header)) != 0) {
        return -1;
    }
    if (avcc_len > 0 && ap_socket_write_all(channel->fd, avcc, avcc_len) != 0) {
        return -1;
    }
    return 0;
}

int ap_data_send_frame(ap_data_channel_t *channel, const void *payload, size_t payload_len, bool idr,
                       uint64_t timestamp, uint64_t timeline_id, uint8_t *scratch,
                       size_t scratch_len)
{
    size_t sealed_len = payload_len + AP_FRAME_TAG_LEN;
    if (scratch == NULL || sealed_len > scratch_len) {
        AP_LOGE("frame of %u bytes does not fit the %u byte scratch buffer", (unsigned)payload_len,
                (unsigned)scratch_len);
        return -1;
    }

    uint8_t header[AP_FRAME_HEADER_LEN];
    ap_frame_build_video(header, sealed_len, idr, timestamp, timeline_id);
    /* The header is the AEAD associated data, so it must be final first. */
    if (hap_aead_seal(channel->key, channel->nonce, header, sizeof(header), payload, payload_len,
                      scratch) != 0) {
        AP_LOGE("frame seal failed (nonce %u)", (unsigned)channel->nonce);
        return -1;
    }
    channel->nonce++;

    if (ap_socket_write_all(channel->fd, header, sizeof(header)) != 0) {
        return -1;
    }
    if (ap_socket_write_all(channel->fd, scratch, sealed_len) != 0) {
        return -1;
    }
    return 0;
}

int ap_data_send_heartbeat(ap_data_channel_t *channel)
{
    uint8_t header[AP_FRAME_HEADER_LEN];
    ap_frame_build_heartbeat(header);
    return ap_socket_write_all(channel->fd, header, sizeof(header));
}
