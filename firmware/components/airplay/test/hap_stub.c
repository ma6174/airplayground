/* Plaintext stand-in for the hap component's channel API.
 *
 * The host tests exercise request construction and response parsing, not the
 * ChaCha20-Poly1305 framing (that belongs to hap). These functions therefore
 * move bytes over the fd unchanged.
 */

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "hap.h"

void hap_channel_init(hap_channel_t *channel, int fd)
{
    memset(channel, 0, sizeof(*channel));
    channel->fd = fd;
}

void hap_channel_enable_encryption(hap_channel_t *channel, const uint8_t write_key[HAP_KEY_LEN],
                                   const uint8_t read_key[HAP_KEY_LEN])
{
    (void)write_key;
    (void)read_key;
    channel->encrypted = true;
}

int hap_channel_write(hap_channel_t *channel, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    while (len > 0) {
        ssize_t written = write(channel->fd, bytes, len);
        if (written <= 0) {
            if (written < 0 && errno == EINTR) {
                continue;
            }
            return -1;
        }
        bytes += (size_t)written;
        len -= (size_t)written;
    }
    return 0;
}

int hap_channel_read(hap_channel_t *channel, void *data, size_t len)
{
    uint8_t *bytes = (uint8_t *)data;
    while (len > 0) {
        ssize_t received = read(channel->fd, bytes, len);
        if (received <= 0) {
            if (received < 0 && errno == EINTR) {
                continue;
            }
            return -1;
        }
        bytes += (size_t)received;
        len -= (size_t)received;
    }
    return 0;
}
