#include "ap_legacy.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ap_client.h"
#include "ap_platform.h"
#include "ap_plist.h"
#include "fairplay.h"

static const char *TAG = "airplay";

/* ------------------------------------------------------------------ CTR */

/* One AES-128 encryption of a single block. On the device this goes through
 * PSA; the host build overrides it (CommonCrypto) so the same code can be
 * tested against the RPiPlay-derived vectors. */
#if defined(AP_HOST_BUILD)
void ap_legacy_aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
#else
#include "psa/crypto.h"

void ap_legacy_aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    static bool psa_ready;
    if (!psa_ready && psa_crypto_init() != PSA_SUCCESS) {
        memset(out, 0, 16);
        return;
    }
    psa_ready = true;

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECB_NO_PADDING);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);

    psa_key_id_t key_id = 0;
    size_t out_len = 0;
    if (psa_import_key(&attributes, key, 16, &key_id) != PSA_SUCCESS) {
        memset(out, 0, 16);
        return;
    }
    psa_status_t status =
        psa_cipher_encrypt(key_id, PSA_ALG_ECB_NO_PADDING, in, 16, out, 16, &out_len);
    psa_destroy_key(key_id);
    if (status != PSA_SUCCESS || out_len != 16) {
        memset(out, 0, 16);
    }
}
#endif

#if !defined(AP_HOST_BUILD)
/* A session uses only a handful of keys (the stream key plus the pair-verify
 * temporaries), so imported handles are cached: importing costs more than
 * encrypting a chunk, and every record would pay for it otherwise. A full
 * cache evicts in round-robin order and the next chunk re-imports that key. */
#define AP_KEY_CACHE_SLOTS 4
static struct {
    uint8_t key[16];
    psa_key_id_t id;
} s_key_cache[AP_KEY_CACHE_SLOTS];
static size_t s_key_cache_next;

static psa_key_id_t key_handle(const uint8_t key[16])
{
    for (size_t i = 0; i < AP_KEY_CACHE_SLOTS; i++) {
        if (s_key_cache[i].id != 0 && memcmp(s_key_cache[i].key, key, 16) == 0) {
            return s_key_cache[i].id;
        }
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECB_NO_PADDING);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);

    psa_key_id_t id = 0;
    if (psa_import_key(&attributes, key, 16, &id) != PSA_SUCCESS) {
        return 0;
    }
    size_t slot = s_key_cache_next;
    s_key_cache_next = (s_key_cache_next + 1) % AP_KEY_CACHE_SLOTS;
    if (s_key_cache[slot].id != 0) {
        psa_destroy_key(s_key_cache[slot].id);
    }
    memcpy(s_key_cache[slot].key, key, 16);
    s_key_cache[slot].id = id;
    return id;
}
#endif

/* Encrypts a batch of independent blocks with a single accelerator call: per
 * block calls turn a 24 KB keyframe into ~150 ms of pure overhead, which
 * starves the frame queue and shows up as visible stutter. */
static void ecb_encrypt_blocks(const uint8_t key[16], const uint8_t *in, uint8_t *out,
                               size_t blocks)
{
#if defined(AP_HOST_BUILD)
    for (size_t i = 0; i < blocks; i++) {
        ap_legacy_aes_block(key, in + 16 * i, out + 16 * i);
    }
#else
    psa_key_id_t id = key_handle(key);
    size_t out_len = 0;
    if (id == 0 ||
        psa_cipher_encrypt(id, PSA_ALG_ECB_NO_PADDING, in, blocks * 16, out, blocks * 16,
                           &out_len) != PSA_SUCCESS ||
        out_len != blocks * 16) {
        memset(out, 0, blocks * 16);
    }
#endif
}

static void counter_increment(uint8_t counter[16])
{
    for (int i = 15; i >= 0; i--) {
        if (++counter[i] != 0) {
            break;
        }
    }
}

/* Fills keystream[] with the next chunk. The counter blocks are staged in a
 * static buffer: CTR contexts belong to one task each (the session task owns
 * the stream context, pair-verify uses a stack context) and are never touched
 * concurrently. */
static void keystream_refill(ap_legacy_ctr_t *ctr)
{
    static uint8_t counters[sizeof(ctr->keystream)];
    for (size_t block = 0; block < AP_LEGACY_CTR_CHUNK_BLOCKS; block++) {
        memcpy(counters + 16 * block, ctr->counter, 16);
        counter_increment(ctr->counter);
    }
    ecb_encrypt_blocks(ctr->key, counters, ctr->keystream, AP_LEGACY_CTR_CHUNK_BLOCKS);
    ctr->used = 0;
}

void ap_legacy_ctr_start(ap_legacy_ctr_t *ctr, const uint8_t key[16], const uint8_t iv[16])
{
    memcpy(ctr->key, key, 16);
    memcpy(ctr->counter, iv, 16);
    ctr->used = sizeof(ctr->keystream);
}

void ap_legacy_ctr_xor(ap_legacy_ctr_t *ctr, uint8_t *data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        if (ctr->used == sizeof(ctr->keystream)) {
            keystream_refill(ctr);
        }
        size_t take = sizeof(ctr->keystream) - ctr->used;
        if (take > len - offset) {
            take = len - offset;
        }
        for (size_t i = 0; i < take; i++) {
            data[offset + i] ^= ctr->keystream[ctr->used + i];
        }
        ctr->used += take;
        offset += take;
    }
}

/* ------------------------------------------------------- key derivations */

static void derive_labelled(const char *label, const uint8_t mixed[16], uint8_t out[16])
{
    /* SHA512(<label><decimal connection id><mixed>)[0..16]; the label already
     * carries the connection id as decimal ASCII (RPiPlay lib/mirror_buffer.c:52-66). */
    uint8_t digest[64];
    size_t label_len = strlen(label);
    uint8_t input[AP_LEGACY_LABEL_MAX + 16];
    memcpy(input, label, label_len);
    memcpy(input + label_len, mixed, 16);
    hap_sha512(input, label_len + 16, digest);
    memcpy(out, digest, 16);
}

void ap_legacy_derive_stream_keys(const uint8_t stream_key[16], const uint8_t ecdh[32],
                                  uint8_t mode, uint64_t connection_id, uint8_t skey[16],
                                  uint8_t siv[16])
{
    if (mode == 3) {
        /* No derivation at all: the AES key is the stream key and the caller
         * supplies the IV. */
        memcpy(skey, stream_key, 16);
        return;
    }
    static const uint8_t zeros[32];
    /* ekey' = SHA512(stream_key || ecdh)[0..16] - the same mixing primitive the
     * HAP path uses, with the legacy pair-verify secret in place of the HAP one.
     * Receivers without FairPlay get the bare key (no ecdh to mix in). */
    uint8_t mixed[16];
    if (mode == 1 && ecdh != NULL) {
        fp_stream_key(stream_key, ecdh, 32, true, mixed);
    } else if (mode == 2) {
        fp_stream_key(stream_key, zeros, sizeof(zeros), true, mixed);
    } else {
        fp_stream_key(stream_key, NULL, 0, false, mixed);
    }

    char label[AP_LEGACY_LABEL_MAX];
    snprintf(label, sizeof(label), "AirPlayStreamKey%llu", (unsigned long long)connection_id);
    derive_labelled(label, mixed, skey);
    snprintf(label, sizeof(label), "AirPlayStreamIV%llu", (unsigned long long)connection_id);
    derive_labelled(label, mixed, siv);
}

/* ------------------------------------------------------- legacy pairing */

/* POST /pair-setup: our Ed25519 public key in, the receiver's out
 * (RPiPlay lib/raop_handlers.h:177-190). */
int ap_legacy_pair_setup(ap_client_t *client, const hap_identity_t *identity,
                         uint8_t receiver_ed[32])
{
    ap_response_t response;
    if (ap_client_request(client, "POST", "/pair-setup", "application/octet-stream",
                          identity->public_key, 32, NULL, &response) != 0) {
        AP_LOGE("legacy pair-setup failed (status %d)", response.status);
        return -1;
    }
    size_t len = 0;
    const uint8_t *body = ap_client_body(client, &len);
    if (len != 32) {
        AP_LOGE("legacy pair-setup replied %u bytes, expected 32", (unsigned)len);
        return -1;
    }
    memcpy(receiver_ed, body, 32);
    return 0;
}

/* Both pair-verify exchanges (RPiPlay lib/raop_handlers.h:213-249 and
 * lib/pairing.c). receiver_ed comes from ap_legacy_pair_setup; on success
 * ecdh_out carries the X25519 secret that the stream key is mixed with. */
int ap_legacy_pair_verify(ap_client_t *client, const hap_identity_t *identity,
                          const uint8_t receiver_ed[32], uint8_t ecdh_out[32])
{
    uint8_t my_x25519_pub[32];
    uint8_t my_x25519_priv[32];
    hap_x25519_keypair(my_x25519_pub, my_x25519_priv);

    /* V1: 0x01, padding, X25519 pub, Ed25519 pub. */
    uint8_t v1[68] = { 0x01, 0, 0, 0 };
    memcpy(v1 + 4, my_x25519_pub, 32);
    memcpy(v1 + 36, identity->public_key, 32);

    ap_response_t response;
    if (ap_client_request(client, "POST", "/pair-verify", "application/octet-stream", v1,
                          sizeof(v1), NULL, &response) != 0) {
        AP_LOGE("legacy pair-verify V1 failed (status %d)", response.status);
        return -1;
    }
    size_t len = 0;
    const uint8_t *body = ap_client_body(client, &len);
    if (len != 96) {
        AP_LOGE("legacy pair-verify V1 replied %u bytes, expected 96", (unsigned)len);
        return -1;
    }

    const uint8_t *peer_x25519 = body;
    if (hap_x25519_secret(ecdh_out, my_x25519_priv, peer_x25519) != 0) {
        AP_LOGE("legacy pair-verify: degenerate X25519 secret");
        return -1;
    }

    /* Signature block keys: SHA512(label || secret)[0..16], concatenation with
     * no padding (RPiPlay lib/pairing.c:25-26,41-55). */
    uint8_t pv_key[16];
    uint8_t pv_iv[16];
    uint8_t digest[64];
    uint8_t material[19 + 32];
    memcpy(material, "Pair-Verify-AES-Key", 19);
    memcpy(material + 19, ecdh_out, 32);
    hap_sha512(material, sizeof(material), digest);
    memcpy(pv_key, digest, 16);
    uint8_t iv_material[18 + 32];
    memcpy(iv_material, "Pair-Verify-AES-IV", 18);
    memcpy(iv_material + 18, ecdh_out, 32);
    hap_sha512(iv_material, sizeof(iv_material), digest);
    memcpy(pv_iv, digest, 16);

    /* The receiver's signature over peerX25519 || ourX25519, sealed with the
     * counter starting at the raw IV (pairing.c:187-199). */
    ap_legacy_ctr_t ctr;
    ap_legacy_ctr_start(&ctr, pv_key, pv_iv);
    uint8_t sealed[64];
    memcpy(sealed, body + 32, 64);
    ap_legacy_ctr_xor(&ctr, sealed, 64);

    uint8_t receiver_signed[64];
    memcpy(receiver_signed, peer_x25519, 32);
    memcpy(receiver_signed + 32, my_x25519_pub, 32);
    if (hap_ed25519_verify(receiver_ed, receiver_signed, sizeof(receiver_signed), sealed) != 0) {
        AP_LOGE("legacy pair-verify: receiver signature check failed");
        return -1;
    }

    /* V2: our signature over ourX25519 || peerX25519, sealed with the
     * keystream advanced by one signature-sized fake round (pairing.c:222-227). */
    uint8_t my_sig[64];
    uint8_t my_signed[64];
    memcpy(my_signed, my_x25519_pub, 32);
    memcpy(my_signed + 32, peer_x25519, 32);
    hap_identity_sign(identity, my_signed, sizeof(my_signed), my_sig);

    ap_legacy_ctr_t ctr2;
    ap_legacy_ctr_start(&ctr2, pv_key, pv_iv);
    uint8_t skip[64];
    ap_legacy_ctr_xor(&ctr2, skip, sizeof(skip));

    uint8_t v2[68] = { 0x00, 0, 0, 0 };
    memcpy(v2 + 4, my_sig, 64);
    ap_legacy_ctr_xor(&ctr2, v2 + 4, 64);

    if (ap_client_request(client, "POST", "/pair-verify", "application/octet-stream", v2,
                          sizeof(v2), NULL, &response) != 0) {
        AP_LOGE("legacy pair-verify V2 failed (status %d)", response.status);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ data records */

int ap_legacy_write_record(int data_fd, bool interleaved, uint8_t channel,
                           const uint8_t header[AP_FRAME_HEADER_LEN], const uint8_t *payload,
                           size_t payload_len)
{
    if (!interleaved) {
        if (ap_socket_write_all(data_fd, header, AP_FRAME_HEADER_LEN) != 0) {
            return -1;
        }
        return payload_len == 0 ? 0 : ap_socket_write_all(data_fd, payload, payload_len);
    }

    /* RTSP interleaved binary: 0x24, channel, big-endian 16-bit length. The
     * interleaved stream normally rides its own connection to the RTSP port;
     * data_fd is that connection. */
    size_t total = AP_FRAME_HEADER_LEN + payload_len;
    if (total > 0xffff) {
        return -1;
    }
    uint8_t prefix[4] = { 0x24, channel, (uint8_t)(total >> 8), (uint8_t)total };
    if (ap_socket_write_all(data_fd, prefix, sizeof(prefix)) != 0 ||
        ap_socket_write_all(data_fd, header, AP_FRAME_HEADER_LEN) != 0) {
        AP_LOGE("interleaved write failed: %s", strerror(errno));
        return -1;
    }
    if (payload_len == 0) {
        return 0;
    }
    if (ap_socket_write_all(data_fd, payload, payload_len) != 0) {
        AP_LOGE("interleaved payload write failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------- session bodies */

size_t ap_legacy_build_announce(char *out, size_t cap, const char *session_uuid,
                                const char *sender_ip, const char *receiver_ip)
{
    /* RPiPlay never parses ANNOUNCE; the fields follow the iOS template so
     * stricter receivers still accept it. The codec detail rides in the
     * mirror stream's avcC record instead of the SDP. */
    return (size_t)snprintf(out, cap,
                            "v=0\r\n"
                            "o=iTunes %s 0 IN IP4 %s\r\n"
                            "s=iTunes\r\n"
                            "c=IN IP4 %s\r\n"
                            "t=0 0\r\n"
                            "m=video 0 RTP/AVP 96\r\n"
                            "a=rtpmap:96 H264/90000\r\n",
                            session_uuid, sender_ip, receiver_ip);
}

/* Small static pools: the legacy SETUP trees are a handful of values built
 * from one task, mirroring ap_setup.c's approach. */
#define AP_LEGACY_VALUE_POOL 72
#define AP_LEGACY_ENTRY_POOL 52
void ap_legacy_audio_keys(const uint8_t stream_key[16], uint8_t bare[16], uint8_t mixed[16])
{
    static const uint8_t zeros[32];
    memcpy(bare, stream_key, 16);
    fp_stream_key(stream_key, zeros, sizeof(zeros), true, mixed);
}

int ap_legacy_build_audio(uint8_t *out, size_t cap, uint16_t seq, uint32_t timestamp,
                          const uint8_t *frame, size_t frame_len, const uint8_t key[16],
                          const uint8_t iv[16])
{
    if (frame == NULL || frame_len == 0 || cap < 12 + frame_len) {
        return -1;
    }
    out[0] = 0x80;
    out[1] = 0x60; /* payload type 96 */
    out[2] = (uint8_t)(seq >> 8);
    out[3] = (uint8_t)seq;
    out[4] = (uint8_t)(timestamp >> 24);
    out[5] = (uint8_t)(timestamp >> 16);
    out[6] = (uint8_t)(timestamp >> 8);
    out[7] = (uint8_t)timestamp;
    memset(out + 8, 0, 4); /* SSRC */
    memcpy(out + 12, frame, frame_len);

#if defined(AP_HOST_BUILD)
    /* The host tests only check the framing, not the CBC step. */
    (void)key;
    (void)iv;
#else
    size_t aligned = frame_len / 16 * 16;
    if (aligned > 0) {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT);
        psa_set_key_algorithm(&attributes, PSA_ALG_CBC_NO_PADDING);
        psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&attributes, 128);
        psa_key_id_t key_id = 0;
        if (psa_import_key(&attributes, key, 16, &key_id) != PSA_SUCCESS) {
            return -1;
        }
        psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
        size_t produced = 0;
        psa_status_t status = psa_cipher_encrypt_setup(&op, key_id, PSA_ALG_CBC_NO_PADDING);
        if (status == PSA_SUCCESS) {
            status = psa_cipher_set_iv(&op, iv, 16);
        }
        if (status == PSA_SUCCESS) {
            status = psa_cipher_update(&op, out + 12, aligned, out + 12, aligned, &produced);
        }
        if (status == PSA_SUCCESS) {
            status = psa_cipher_finish(&op, out + 12 + produced, 16, &produced);
        }
        psa_cipher_abort(&op);
        psa_destroy_key(key_id);
        if (status != PSA_SUCCESS) {
            return -1;
        }
    }
#endif
    return (int)(12 + frame_len);
}

static ap_plist_value_t s_values[AP_LEGACY_VALUE_POOL];
static ap_plist_entry_t s_entries[AP_LEGACY_ENTRY_POOL];
static size_t s_value_used;
static size_t s_entry_used;

static void pool_reset(void)
{
    s_value_used = 0;
    s_entry_used = 0;
}

static ap_plist_value_t *value_slot(void)
{
    if (s_value_used >= AP_LEGACY_VALUE_POOL) {
        return NULL;
    }
    ap_plist_value_t *value = &s_values[s_value_used++];
    memset(value, 0, sizeof(*value));
    return value;
}

/* Contiguous run of slots: array items must be stored next to each other. */
static ap_plist_value_t *value_slots(size_t count)
{
    if (s_value_used + count > AP_LEGACY_VALUE_POOL) {
        return NULL;
    }
    ap_plist_value_t *first = &s_values[s_value_used];
    s_value_used += count;
    memset(first, 0, count * sizeof(*first));
    return first;
}

static ap_plist_entry_t *entry_slots(size_t count)
{
    if (s_entry_used + count > AP_LEGACY_ENTRY_POOL) {
        return NULL;
    }
    ap_plist_entry_t *first = &s_entries[s_entry_used];
    s_entry_used += count;
    return first;
}

static ap_plist_value_t *make_uint(uint64_t number)
{
    ap_plist_value_t *value = value_slot();
    if (value != NULL) {
        value->type = AP_PLIST_INT;
        value->as.integer = (int64_t)number;
    }
    return value;
}

static ap_plist_value_t *make_data(const void *ptr, size_t len)
{
    ap_plist_value_t *value = value_slot();
    if (value != NULL) {
        value->type = AP_PLIST_DATA;
        value->as.bytes.ptr = ptr;
        value->as.bytes.len = len;
    }
    return value;
}

static ap_plist_value_t *make_bool(bool value)
{
    ap_plist_value_t *slot = value_slot();
    if (slot != NULL) {
        slot->type = AP_PLIST_BOOL;
        slot->as.boolean = value;
    }
    return slot;
}

static ap_plist_value_t *make_string(const char *text)
{
    ap_plist_value_t *slot = value_slot();
    if (slot != NULL) {
        slot->type = AP_PLIST_STRING;
        slot->as.bytes.ptr = text;
        slot->as.bytes.len = strlen(text);
    }
    return slot;
}

static ap_plist_value_t *make_dict(ap_plist_entry_t *entries, size_t count)
{
    ap_plist_value_t *value = value_slot();
    if (value != NULL) {
        value->type = AP_PLIST_DICT;
        value->as.dict.entries = entries;
        value->as.dict.count = count;
    }
    return value;
}

size_t ap_legacy_build_setup1(uint8_t *out, size_t cap, const ap_legacy_setup1_t *setup)
{
    pool_reset();
    ap_plist_entry_t *entries = entry_slots(20);
    if (entries == NULL) {
        return 0;
    }
    size_t count = 0;
    entries[count++] = (ap_plist_entry_t){ "eiv", make_data(setup->eiv, 16) };
    if (setup->ekey != NULL) {
        entries[count++] = (ap_plist_entry_t){ "ekey", make_data(setup->ekey, 72) };
    }
    entries[count++] = (ap_plist_entry_t){ "timingPort", make_uint(setup->timing_port) };
    entries[count++] = (ap_plist_entry_t){ "et", make_uint(32) };
    entries[count++] = (ap_plist_entry_t){ "timingProtocol", make_string("NTP") };
    entries[count++] = (ap_plist_entry_t){ "isScreenMirroringSession", make_bool(true) };
    entries[count++] = (ap_plist_entry_t){ "statsCollectionEnabled", make_bool(false) };
    /* Receivers of this class sniff the sender identity before they arm the
     * mirroring pipeline. */
    entries[count++] = (ap_plist_entry_t){ "sourceVersion", make_string("930.7.1") };
    entries[count++] = (ap_plist_entry_t){ "model", make_string("iPhone17,1") };
    entries[count++] = (ap_plist_entry_t){ "name", make_string("esp32") };
    entries[count++] = (ap_plist_entry_t){ "osName", make_string("iPhone OS") };
    entries[count++] = (ap_plist_entry_t){ "osVersion", make_string("26.0") };
    entries[count++] = (ap_plist_entry_t){ "osBuildVersion", make_string("23A344") };
    entries[count++] = (ap_plist_entry_t){ "updateSessionRequest", make_bool(false) };
    entries[count++] = (ap_plist_entry_t){ "combinedGetInfoWithControlSetup", make_bool(true) };
    if (setup->session_uuid != NULL) {
        entries[count++] = (ap_plist_entry_t){ "sessionUUID", make_string(setup->session_uuid) };
    }
    if (setup->session_correlation_uuid != NULL) {
        entries[count++] = (ap_plist_entry_t){ "sessionCorrelationUUID",
                                              make_string(setup->session_correlation_uuid) };
    }
    if (setup->device_id != NULL) {
        entries[count++] = (ap_plist_entry_t){ "deviceID", make_string(setup->device_id) };
    }
    if (setup->mac_address != NULL) {
        entries[count++] = (ap_plist_entry_t){ "macAddress", make_string(setup->mac_address) };
    }
    ap_plist_value_t *root = make_dict(entries, count);
    return root == NULL ? 0 : ap_plist_encode(root, out, cap);
}

size_t ap_legacy_build_setup2(uint8_t *out, size_t cap, uint64_t connection_id, bool with_audio,
                              uint16_t audio_control_port, uint16_t audio_spf)
{
    pool_reset();
    size_t stream_count = with_audio ? 2 : 1;
    /* Array items must be contiguous values: take both slots before filling. */
    ap_plist_value_t *items = value_slot();
    if (items == NULL || (with_audio && value_slot() == NULL)) {
        return 0;
    }

    /* The sender states which timestamps its frame header carries; receivers
     * use the list to find them in the header. */
    static const char *const stamp_names[] = { "SubSu", "BePxT", "AfPxT", "BefEn", "EmEnc" };
    const size_t stamp_count = sizeof(stamp_names) / sizeof(stamp_names[0]);
    ap_plist_value_t *stamp_items = value_slots(stamp_count);
    if (stamp_items == NULL) {
        return 0;
    }
    for (size_t i = 0; i < stamp_count; i++) {
        ap_plist_entry_t *entry = entry_slots(1);
        if (entry == NULL) {
            return 0;
        }
        entry[0] = (ap_plist_entry_t){ "name", make_string(stamp_names[i]) };
        const ap_plist_value_t *item = make_dict(entry, 1);
        if (item == NULL) {
            return 0;
        }
        stamp_items[i] = *item;
    }
    ap_plist_value_t *stamps = value_slot();
    if (stamps == NULL) {
        return 0;
    }
    stamps->type = AP_PLIST_ARRAY;
    stamps->as.array.items = stamp_items;
    stamps->as.array.count = stamp_count;

    ap_plist_entry_t *video_entries = entry_slots(4);
    if (video_entries == NULL) {
        return 0;
    }
    video_entries[0] = (ap_plist_entry_t){ "type", make_uint(110) };
    video_entries[1] = (ap_plist_entry_t){ "streamConnectionID", make_uint(connection_id) };
    video_entries[2] = (ap_plist_entry_t){ "timestampInfo", stamps };
    video_entries[3] = (ap_plist_entry_t){ "latencyMs", make_uint(100) };
    const ap_plist_value_t *video = make_dict(video_entries, 4);
    if (video == NULL) {
        return 0;
    }
    items[0] = *video;

    if (with_audio) {
        /* The audio stream's own dataPort comes back in the reply. The rest has
         * to be stated here: without the sample rate the receiver falls back to
         * its own default and plays the stream at the wrong speed, and without a
         * buffer window every late packet leaves an audible hole. */
        ap_plist_entry_t *audio_entries = entry_slots(13);
        if (audio_entries == NULL) {
            return 0;
        }
        audio_entries[0] = (ap_plist_entry_t){ "type", make_uint(96) };
        audio_entries[1] = (ap_plist_entry_t){ "streamConnectionID", make_uint(connection_id + 1) };
        audio_entries[2] = (ap_plist_entry_t){ "audioFormat", make_uint(0x40000) };
        audio_entries[3] = (ap_plist_entry_t){ "controlPort", make_uint(audio_control_port) };
        audio_entries[4] = (ap_plist_entry_t){ "ct", make_uint(2) };
        audio_entries[5] = (ap_plist_entry_t){ "spf", make_uint(audio_spf) };
        audio_entries[6] = (ap_plist_entry_t){ "sr", make_uint(44100) };
        audio_entries[7] = (ap_plist_entry_t){ "ss", make_uint(16) };
        audio_entries[8] = (ap_plist_entry_t){ "ch", make_uint(2) };
        audio_entries[9] = (ap_plist_entry_t){ "audioMode", make_string("default") };
        audio_entries[10] = (ap_plist_entry_t){ "usingScreen", make_bool(true) };
        /* Buffer window in samples: 100 ms to 400 ms of slack for the receiver. */
        audio_entries[11] = (ap_plist_entry_t){ "latencyMin", make_uint(4410) };
        audio_entries[12] = (ap_plist_entry_t){ "latencyMax", make_uint(17640) };
        const ap_plist_value_t *audio_stream = make_dict(audio_entries, 13);
        if (audio_stream == NULL) {
            return 0;
        }
        items[1] = *audio_stream;
    }

    ap_plist_value_t *array = value_slot();
    if (array == NULL) {
        return 0;
    }
    array->type = AP_PLIST_ARRAY;
    array->as.array.items = items;
    array->as.array.count = stream_count;

    ap_plist_entry_t *root_entries = entry_slots(1);
    if (root_entries == NULL) {
        return 0;
    }
    root_entries[0] = (ap_plist_entry_t){ "streams", array };
    ap_plist_value_t *root = make_dict(root_entries, 1);
    return root == NULL ? 0 : ap_plist_encode(root, out, cap);
}
