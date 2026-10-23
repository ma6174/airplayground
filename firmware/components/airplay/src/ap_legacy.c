#include "ap_legacy.h"

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

static void counter_increment(uint8_t counter[16])
{
    for (int i = 15; i >= 0; i--) {
        if (++counter[i] != 0) {
            break;
        }
    }
}

void ap_legacy_ctr_start(ap_legacy_ctr_t *ctr, const uint8_t key[16], const uint8_t iv[16])
{
    memcpy(ctr->key, key, 16);
    memcpy(ctr->counter, iv, 16);
    ap_legacy_aes_block(ctr->key, ctr->counter, ctr->keystream);
    ctr->used = 0;
}

void ap_legacy_ctr_xor(ap_legacy_ctr_t *ctr, uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (ctr->used == 16) {
            counter_increment(ctr->counter);
            ap_legacy_aes_block(ctr->key, ctr->counter, ctr->keystream);
            ctr->used = 0;
        }
        data[i] ^= ctr->keystream[ctr->used++];
    }
}

/* ------------------------------------------------------- key derivations */

static void derive_labelled(const char *label, const uint8_t mixed[16], uint8_t out[16])
{
    /* SHA512(<label><decimal connection id><mixed>)[0..16]; the label already
     * carries the connection id as decimal ASCII (RPiPlay lib/mirror_buffer.c:52-66). */
    uint8_t digest[64];
    size_t label_len = strlen(label);
    uint8_t input[24 + 16];
    memcpy(input, label, label_len);
    memcpy(input + label_len, mixed, 16);
    hap_sha512(input, label_len + 16, digest);
    memcpy(out, digest, 16);
}

void ap_legacy_derive_stream_keys(const uint8_t stream_key[16], const uint8_t ecdh[32],
                                  uint64_t connection_id, uint8_t skey[16], uint8_t siv[16])
{
    /* ekey' = SHA512(stream_key || ecdh)[0..16] - the same mixing primitive the
     * HAP path uses, with the legacy pair-verify secret in place of the HAP one. */
    uint8_t mixed[16];
    fp_stream_key(stream_key, ecdh, 32, true, mixed);

    char label[32];
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
     * keystream advanced by one block (pairing.c:222-227, "one fake round"). */
    uint8_t my_sig[64];
    uint8_t my_signed[64];
    memcpy(my_signed, my_x25519_pub, 32);
    memcpy(my_signed + 32, peer_x25519, 32);
    hap_identity_sign(identity, my_signed, sizeof(my_signed), my_sig);

    ap_legacy_ctr_t ctr2;
    ap_legacy_ctr_start(&ctr2, pv_key, pv_iv);
    uint8_t skip[16];
    ap_legacy_ctr_xor(&ctr2, skip, 16);

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
#define AP_LEGACY_VALUE_POOL 8
#define AP_LEGACY_ENTRY_POOL 6
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

size_t ap_legacy_build_setup1(uint8_t *out, size_t cap, const uint8_t eiv[16],
                              const uint8_t ekey[72], uint16_t timing_port)
{
    pool_reset();
    ap_plist_entry_t *entries = entry_slots(3);
    if (entries == NULL) {
        return 0;
    }
    entries[0] = (ap_plist_entry_t){ "eiv", make_data(eiv, 16) };
    entries[1] = (ap_plist_entry_t){ "ekey", make_data(ekey, 72) };
    entries[2] = (ap_plist_entry_t){ "timingPort", make_uint(timing_port) };
    ap_plist_value_t *root = make_dict(entries, 3);
    return root == NULL ? 0 : ap_plist_encode(root, out, cap);
}

size_t ap_legacy_build_setup2(uint8_t *out, size_t cap, uint64_t connection_id)
{
    pool_reset();
    ap_plist_entry_t *stream_entries = entry_slots(2);
    if (stream_entries == NULL) {
        return 0;
    }
    stream_entries[0] = (ap_plist_entry_t){ "type", make_uint(110) };
    stream_entries[1] = (ap_plist_entry_t){ "streamConnectionID", make_uint(connection_id) };
    ap_plist_value_t *stream = make_dict(stream_entries, 2);

    ap_plist_value_t *array = value_slot();
    if (stream == NULL || array == NULL) {
        return 0;
    }
    array->type = AP_PLIST_ARRAY;
    array->as.array.items = stream;
    array->as.array.count = 1;

    ap_plist_entry_t *root_entries = entry_slots(1);
    if (root_entries == NULL) {
        return 0;
    }
    root_entries[0] = (ap_plist_entry_t){ "streams", array };
    ap_plist_value_t *root = make_dict(root_entries, 1);
    return root == NULL ? 0 : ap_plist_encode(root, out, cap);
}
