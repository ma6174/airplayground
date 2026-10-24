/*
 * ChaCha20-Poly1305 sealing and the HAP framing used once pair-verify succeeds.
 *
 * Frame layout: [2-byte LE plaintext length][ciphertext][16-byte tag], at most
 * 1024 plaintext bytes per frame, AAD = the length prefix, nonce = 4 zero bytes
 * followed by the little-endian block counter. Write and read counters advance
 * independently and start at zero for each connection.
 */
#include "hap_internal.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "psa/crypto.h"

#define HAP_FRAME_MAX 1024
#define HAP_TAG_LEN 16
#define HAP_NONCE_LEN 12

static void build_nonce(uint8_t nonce[HAP_NONCE_LEN], uint64_t counter)
{
    int index;

    memset(nonce, 0, HAP_NONCE_LEN);
    for (index = 0; index < 8; index++) {
        nonce[4 + index] = (uint8_t)(counter >> (8 * index));
    }
}

static int import_aead_key(const uint8_t key[HAP_KEY_LEN], psa_key_usage_t usage,
                           mbedtls_svc_key_id_t *key_id)
{
    psa_key_attributes_t attributes = psa_key_attributes_init();
    psa_status_t status;

    if (hap_psa_ensure_init() != 0) {
        return -1;
    }
    psa_set_key_type(&attributes, PSA_KEY_TYPE_CHACHA20);
    psa_set_key_bits(&attributes, 8 * HAP_KEY_LEN);
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, PSA_ALG_CHACHA20_POLY1305);

    status = psa_import_key(&attributes, key, HAP_KEY_LEN, key_id);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        if (status == PSA_ERROR_NOT_SUPPORTED) {
            /* The one sdkconfig mistake that looks like a protocol bug. */
            HAP_LOGE("aead: ChaCha20 is missing from mbedtls; enable "
                     "CONFIG_MBEDTLS_CHACHA20_C and CONFIG_MBEDTLS_CHACHAPOLY_C");
        } else {
            HAP_LOGE("aead: key import failed (%ld)", (long)status);
        }
        return -1;
    }
    return 0;
}

static int seal_with_nonce(const uint8_t key[HAP_KEY_LEN], const uint8_t nonce[HAP_NONCE_LEN],
                          const uint8_t *aad, size_t aad_len, const uint8_t *plaintext,
                          size_t plaintext_len, uint8_t *ciphertext)
{
    mbedtls_svc_key_id_t key_id = 0;
    size_t produced = 0;
    psa_status_t status;

    if (key == NULL || plaintext == NULL || ciphertext == NULL || plaintext_len == 0) {
        return -1;
    }
    if (import_aead_key(key, PSA_KEY_USAGE_ENCRYPT, &key_id) != 0) {
        return -1;
    }
    status = psa_aead_encrypt(key_id, PSA_ALG_CHACHA20_POLY1305, nonce, HAP_NONCE_LEN, aad, aad_len,
                             plaintext, plaintext_len, ciphertext, plaintext_len + HAP_TAG_LEN,
                             &produced);
    psa_destroy_key(key_id);
    if (status != PSA_SUCCESS || produced != plaintext_len + HAP_TAG_LEN) {
        HAP_LOGE("aead: seal failed (%ld, %u bytes)", (long)status, (unsigned)produced);
        return -1;
    }
    return 0;
}

static int open_with_nonce(const uint8_t key[HAP_KEY_LEN], const uint8_t nonce[HAP_NONCE_LEN],
                          const uint8_t *aad, size_t aad_len, const uint8_t *ciphertext,
                          size_t ciphertext_len, uint8_t *plaintext)
{
    mbedtls_svc_key_id_t key_id = 0;
    size_t produced = 0;
    psa_status_t status;

    if (key == NULL || ciphertext == NULL || plaintext == NULL || ciphertext_len <= HAP_TAG_LEN) {
        return -1;
    }
    if (import_aead_key(key, PSA_KEY_USAGE_DECRYPT, &key_id) != 0) {
        return -1;
    }
    status = psa_aead_decrypt(key_id, PSA_ALG_CHACHA20_POLY1305, nonce, HAP_NONCE_LEN, aad, aad_len,
                             ciphertext, ciphertext_len, plaintext, ciphertext_len - HAP_TAG_LEN,
                             &produced);
    psa_destroy_key(key_id);
    if (status != PSA_SUCCESS || produced != ciphertext_len - HAP_TAG_LEN) {
        HAP_LOGE("aead: open failed (%ld)", (long)status);
        return -1;
    }
    return 0;
}

int hap_aead_seal(const uint8_t key[HAP_KEY_LEN], uint64_t nonce, const uint8_t *aad,
                  size_t aad_len, const uint8_t *plaintext, size_t plaintext_len,
                  uint8_t *ciphertext)
{
    uint8_t nonce_bytes[HAP_NONCE_LEN];

    build_nonce(nonce_bytes, nonce);
    return seal_with_nonce(key, nonce_bytes, aad, aad_len, plaintext, plaintext_len, ciphertext);
}

int hap_aead_open(const uint8_t key[HAP_KEY_LEN], uint64_t nonce, const uint8_t *aad,
                  size_t aad_len, const uint8_t *ciphertext, size_t ciphertext_len,
                  uint8_t *plaintext)
{
    uint8_t nonce_bytes[HAP_NONCE_LEN];

    build_nonce(nonce_bytes, nonce);
    return open_with_nonce(key, nonce_bytes, aad, aad_len, ciphertext, ciphertext_len, plaintext);
}

/* Builds the pairing-message nonce: 4 zero bytes followed by an 8-byte label. */
static int label_nonce(uint8_t nonce[HAP_NONCE_LEN], const char *label)
{
    if (label == NULL || strlen(label) != 8) {
        HAP_LOGE("aead: pairing label must be 8 characters");
        return -1;
    }
    memset(nonce, 0, 4);
    memcpy(nonce + 4, label, 8);
    return 0;
}

int hap_aead_seal_label(const uint8_t key[HAP_KEY_LEN], const char *label, const uint8_t *aad,
                        size_t aad_len, const uint8_t *plaintext, size_t plaintext_len,
                        uint8_t *ciphertext)
{
    uint8_t nonce[HAP_NONCE_LEN];

    if (label_nonce(nonce, label) != 0) {
        return -1;
    }
    return seal_with_nonce(key, nonce, aad, aad_len, plaintext, plaintext_len, ciphertext);
}

int hap_aead_open_label(const uint8_t key[HAP_KEY_LEN], const char *label, const uint8_t *aad,
                        size_t aad_len, const uint8_t *ciphertext, size_t ciphertext_len,
                        uint8_t *plaintext)
{
    uint8_t nonce[HAP_NONCE_LEN];

    if (label_nonce(nonce, label) != 0) {
        return -1;
    }
    return open_with_nonce(key, nonce, aad, aad_len, ciphertext, ciphertext_len, plaintext);
}

static int write_all(int fd, const void *data, size_t len)
{
    const uint8_t *cursor = data;

    while (len > 0) {
        ssize_t written = write(fd, cursor, len);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            HAP_LOGE("channel: write failed (%d)", errno);
            return -1;
        }
        if (written == 0) {
            HAP_LOGE("channel: write returned zero");
            return -1;
        }
        cursor += (size_t)written;
        len -= (size_t)written;
    }
    return 0;
}

static int read_all(int fd, void *data, size_t len)
{
    uint8_t *cursor = data;

    while (len > 0) {
        ssize_t received = read(fd, cursor, len);

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            HAP_LOGE("channel: read failed (%d)", errno);
            return -1;
        }
        if (received == 0) {
            HAP_LOGE("channel: connection closed while %u bytes were pending", (unsigned)len);
            return -1;
        }
        cursor += (size_t)received;
        len -= (size_t)received;
    }
    return 0;
}

void hap_channel_init(hap_channel_t *channel, int fd)
{
    if (channel == NULL) {
        return;
    }
    memset(channel, 0, sizeof *channel);
    channel->fd = fd;
    channel->encrypted = false;
}

void hap_channel_enable_encryption(hap_channel_t *channel, const uint8_t write_key[HAP_KEY_LEN],
                                   const uint8_t read_key[HAP_KEY_LEN])
{
    if (channel == NULL || write_key == NULL || read_key == NULL) {
        return;
    }
    memcpy(channel->write_key, write_key, HAP_KEY_LEN);
    memcpy(channel->read_key, read_key, HAP_KEY_LEN);
    channel->write_nonce = 0;
    channel->read_nonce = 0;
    channel->encrypted = true;
}

int hap_channel_write(hap_channel_t *channel, const void *data, size_t len)
{
    const uint8_t *cursor = data;

    if (channel == NULL || channel->fd < 0 || (data == NULL && len != 0)) {
        return -1;
    }
    if (!channel->encrypted) {
        return len == 0 ? 0 : write_all(channel->fd, data, len);
    }

    while (len > 0) {
        uint8_t frame[2 + HAP_FRAME_MAX + HAP_TAG_LEN];
        size_t chunk = len > HAP_FRAME_MAX ? HAP_FRAME_MAX : len;

        frame[0] = (uint8_t)(chunk & 0xFFu);
        frame[1] = (uint8_t)(chunk >> 8);
        if (hap_aead_seal(channel->write_key, channel->write_nonce, frame, 2, cursor, chunk,
                          frame + 2) != 0) {
            return -1;
        }
        channel->write_nonce++;
        if (write_all(channel->fd, frame, 2 + chunk + HAP_TAG_LEN) != 0) {
            return -1;
        }
        cursor += chunk;
        len -= chunk;
    }
    return 0;
}

int hap_channel_read(hap_channel_t *channel, void *data, size_t len)
{
    uint8_t *cursor = data;

    if (channel == NULL || channel->fd < 0 || (data == NULL && len != 0)) {
        return -1;
    }
    if (!channel->encrypted) {
        return len == 0 ? 0 : read_all(channel->fd, data, len);
    }

    /* Serve whatever a previous frame left over before touching the socket:
     * callers treat this channel as a byte stream and may ask for one byte at a
     * time while parsing a status line. */
    for (;;) {
        if (channel->pending_offset < channel->pending_len) {
            size_t available = channel->pending_len - channel->pending_offset;
            size_t take = available < len ? available : len;

            memcpy(cursor, channel->pending + channel->pending_offset, take);
            channel->pending_offset += take;
            cursor += take;
            len -= take;
            if (len == 0) {
                return 0;
            }
        }

        {
            uint8_t prefix[2];
            uint8_t sealed[HAP_FRAME_MAX + HAP_TAG_LEN];
            size_t plain_len;

            if (read_all(channel->fd, prefix, sizeof prefix) != 0) {
                return -1;
            }
            plain_len = (size_t)prefix[0] | ((size_t)prefix[1] << 8);
            if (plain_len == 0 || plain_len > HAP_FRAME_MAX) {
                HAP_LOGE("channel: bad frame length %u", (unsigned)plain_len);
                return -1;
            }
            if (read_all(channel->fd, sealed, plain_len + HAP_TAG_LEN) != 0) {
                return -1;
            }
            if (hap_aead_open(channel->read_key, channel->read_nonce, prefix, sizeof prefix, sealed,
                              plain_len + HAP_TAG_LEN, channel->pending) != 0) {
                return -1;
            }
            channel->read_nonce++;
            channel->pending_offset = 0;
            channel->pending_len = plain_len;
        }
    }
}
