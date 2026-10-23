/*
 * Self test: re-derives every published vector on the target and reports the
 * first failure of each category.
 *
 * The vectors themselves live in fp_vectors.c, generated from the Go reference
 * implementation, so a regression here means the port drifted from the code
 * that produced the vectors - not that the test data was retyped wrong.
 *
 * Categories (fp_selftest_run runs them all, fp_selftest wraps that):
 *
 *   platform      SHA-512 / HMAC-SHA1 known answers, stream-key mixing
 *   sap_hash      64 corpus blocks + one deterministic block
 *   md5           swap / cycle / kdf schedule mutations
 *   message       mode 0..3 decrypt, re-encrypt and wrapping key
 *   exchange      descriptor + 20-byte exchange, 7 receiver-SAP shapes
 *   wrapping_key  the four per-mode KDF results
 *   chain         m1 -> m3 -> m4 -> ekey, including HMAC-SHA1 and the AES block
 *   unwrap        captured m3/ekey: wrapping key, sender SAP, mac key, AES block
 *   reject        malformed m2/m4 and out-of-order calls must be refused
 */
#include <stdio.h>
#include <string.h>

#include "fp_internal.h"

/* Upper bound for the hex-decoded blobs below (largest vector input is 200
 * bytes of SHA-512 message data). */
#define FP_HEX_BUF 256

typedef struct {
    fp_test_stats *stats;
    fp_test_category category;
    bool reported;
} fp_checker;

static const char *const fp_category_names[FP_TEST_CATEGORY_COUNT] = {
    "platform", "sap_hash", "md5", "message", "exchange", "wrapping_key", "chain", "unwrap", "reject",
};

const char *fp_test_category_name(fp_test_category category)
{
    if (category < 0 || category >= FP_TEST_CATEGORY_COUNT) {
        return "unknown";
    }
    return fp_category_names[category];
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Strict decoder: the string must be exactly two hex digits per output byte, so
 * a truncated vector fails instead of silently leaving zeros behind. */
static int decode_hex(const char *hex, uint8_t *out, size_t out_len)
{
    if (hex == NULL || out == NULL || strlen(hex) != out_len * 2) {
        return -1;
    }
    for (size_t i = 0; i < out_len; i++) {
        int high = hex_nibble(hex[2 * i]);
        int low = hex_nibble(hex[2 * i + 1]);
        if (high < 0 || low < 0) {
            return -1;
        }
        out[i] = (uint8_t)((high << 4) | low);
    }
    return 0;
}

static void print_hex(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
}

static void report_failure(fp_checker *checker, const char *name, const uint8_t *got, const uint8_t *want,
                           size_t len)
{
    if (checker->reported) {
        return; /* only the first failure of a category is spelled out */
    }
    checker->reported = true;
    printf("[fairplay] FAIL %s / %s\n  got  ", fp_test_category_name(checker->category), name);
    print_hex(got, len);
    printf("\n  want ");
    print_hex(want, len);
    printf("\n");
}

static void check_bytes(fp_checker *checker, const char *name, const uint8_t *got, const uint8_t *want,
                        size_t len)
{
    checker->stats->checked++;
    if (memcmp(got, want, len) != 0) {
        checker->stats->failed++;
        report_failure(checker, name, got, want, len);
    }
}

static void check_int(fp_checker *checker, const char *name, int got, int want)
{
    checker->stats->checked++;
    if (got != want) {
        checker->stats->failed++;
        if (!checker->reported) {
            checker->reported = true;
            printf("[fairplay] FAIL %s / %s: got %d, want %d\n", fp_test_category_name(checker->category),
                   name, got, want);
        }
    }
}

/* Decodes a vector string and fails the check when it is malformed. */
static void load_hex(fp_checker *checker, const char *name, const char *hex, uint8_t *out, size_t out_len)
{
    check_int(checker, name, decode_hex(hex, out, out_len), 0);
}

/* Entropy script: the first draw seeds the local SAP, the second one is the ekey
 * mask. Both come from the vector, so the whole chain is deterministic. */
typedef struct {
    const uint8_t *first;
    size_t first_len;
    const uint8_t *second;
    size_t second_len;
    int calls;
} scripted_random;

static int scripted_random_fn(void *ctx, uint8_t *out, size_t len)
{
    scripted_random *plan = (scripted_random *)ctx;
    const uint8_t *source = (plan->calls == 0) ? plan->first : plan->second;
    size_t source_len = (plan->calls == 0) ? plan->first_len : plan->second_len;

    plan->calls++;
    if (source == NULL || source_len != len) {
        return -1;
    }
    memcpy(out, source, len);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* categories                                                                */
/* ------------------------------------------------------------------------- */

static void run_platform(fp_checker *checker)
{
    uint8_t data[FP_HEX_BUF];
    uint8_t want[FP_SHA512_LEN];
    uint8_t got[FP_SHA512_LEN];

    for (unsigned i = 0; i < fp_vectors_sha512_count; i++) {
        const fp_sha512_vector *v = &fp_vectors_sha512[i];
        size_t len = strlen(v->data) / 2;
        if (len > sizeof data) {
            check_int(checker, "sha512 vector size", -1, 0);
            break;
        }
        load_hex(checker, v->name, v->data, data, len);
        load_hex(checker, v->name, v->digest, want, sizeof want);
        memset(got, 0, sizeof got);
        check_int(checker, v->name, fp_sha512(data, len, got), FP_OK);
        check_bytes(checker, v->name, got, want, sizeof want);
    }

    for (unsigned i = 0; i < fp_vectors_hmac_sha1_count; i++) {
        const fp_mac_vector *v = &fp_vectors_hmac_sha1[i];
        uint8_t key[FP_HEX_BUF];
        uint8_t msg[FP_HEX_BUF];
        uint8_t want_tag[FP_SHA1_LEN];
        uint8_t got_tag[FP_SHA1_LEN];
        size_t key_len = strlen(v->key) / 2;
        size_t msg_len = strlen(v->msg) / 2;
        if (key_len > sizeof key || msg_len > sizeof msg) {
            check_int(checker, "hmac vector size", -1, 0);
            break;
        }
        load_hex(checker, v->name, v->key, key, key_len);
        load_hex(checker, v->name, v->msg, msg, msg_len);
        load_hex(checker, v->name, v->digest, want_tag, sizeof want_tag);
        memset(got_tag, 0, sizeof got_tag);
        check_int(checker, v->name, fp_hmac_sha1(key, key_len, msg, msg_len, got_tag), FP_OK);
        check_bytes(checker, v->name, got_tag, want_tag, sizeof want_tag);
    }

    for (unsigned i = 0; i < fp_vectors_stream_key_count; i++) {
        const fp_stream_key_vector *v = &fp_vectors_stream_key[i];
        uint8_t raw_key[FP_RAW_KEY_LEN];
        uint8_t secret[FP_PAIR_SECRET_MAX];
        uint8_t want_key[FP_RAW_KEY_LEN];
        uint8_t got_key[FP_RAW_KEY_LEN];
        size_t secret_len = strlen(v->pair_secret) / 2;
        if (secret_len > sizeof secret) {
            check_int(checker, "stream key vector size", -1, 0);
            break;
        }
        load_hex(checker, v->name, v->raw_key, raw_key, sizeof raw_key);
        load_hex(checker, v->name, v->pair_secret, secret, secret_len);
        load_hex(checker, v->name, v->stream_key, want_key, sizeof want_key);
        fp_stream_key(raw_key, secret, secret_len, true, got_key);
        check_bytes(checker, v->name, got_key, want_key, sizeof want_key);
        /* Without pair-verify the raw key is the stream key. */
        fp_stream_key(raw_key, NULL, 0, false, got_key);
        check_bytes(checker, "unmixed stream key", got_key, raw_key, sizeof raw_key);
    }
}

static void run_sap_hash(fp_checker *checker)
{
    uint8_t block[64];
    uint8_t want[16];
    uint8_t got[16];

    for (unsigned i = 0; i < fp_vectors_sap_hash_count; i++) {
        const fp_hash_vector *v = &fp_vectors_sap_hash[i];
        load_hex(checker, v->name, v->block, block, sizeof block);
        load_hex(checker, v->name, v->digest, want, sizeof want);
        memset(got, 0, sizeof got);
        fp_sap_hash(block, got);
        check_bytes(checker, v->name, got, want, sizeof want);
    }
}

static void run_md5(fp_checker *checker)
{
    uint8_t block[64];
    uint8_t want[16];
    uint8_t got[16];
    /* The published vectors compress this block with a little-endian seeded
     * state of 00 01 02 .. 0f. */
    uint8_t key[16];

    for (int i = 0; i < 16; i++) {
        key[i] = (uint8_t)i;
    }
    for (unsigned i = 0; i < fp_vectors_md5_count; i++) {
        const fp_md5_vector *v = &fp_vectors_md5[i];
        uint32_t state[4];
        load_hex(checker, v->name, v->block, block, sizeof block);
        load_hex(checker, v->name, v->digest, want, sizeof want);
        for (int word = 0; word < 4; word++) {
            state[word] = fp_rd_le32(key + word * 4);
        }
        fp_md5_compress(state, block, v->mutation);
        for (int word = 0; word < 4; word++) {
            fp_wr_be32(got + word * 4, state[word]);
        }
        check_bytes(checker, v->name, got, want, sizeof want);
    }
}

static void run_message(fp_checker *checker)
{
    uint8_t cipher[FP_BODY_LEN];
    uint8_t plain[FP_BODY_LEN];
    uint8_t m3[FP_M3_LEN];
    uint8_t produced[FP_BODY_LEN];

    for (unsigned i = 0; i < fp_vectors_message_count; i++) {
        const fp_message_vector *v = &fp_vectors_message[i];
        load_hex(checker, v->name, v->cipher, cipher, sizeof cipher);
        load_hex(checker, v->name, v->plain, plain, sizeof plain);
        load_hex(checker, v->name, v->m3, m3, sizeof m3);
        char name[64];

        snprintf(name, sizeof name, "%s mode byte", v->name);
        check_int(checker, name, m3[12], v->mode);
        memset(produced, 0, sizeof produced);
        snprintf(name, sizeof name, "%s decrypt", v->name);
        check_int(checker, name, fp_message_decrypt(m3[12], m3 + 16, produced), FP_OK);
        check_bytes(checker, v->name, produced, plain, sizeof plain);
        memset(produced, 0, sizeof produced);
        snprintf(name, sizeof name, "%s encrypt", v->name);
        check_int(checker, name, fp_message_encrypt(v->mode, plain, produced), FP_OK);
        check_bytes(checker, v->name, produced, cipher, sizeof cipher);
    }
}

static void run_exchange(fp_checker *checker)
{
    uint8_t initiator[FP_SAP_LEN];
    uint8_t receiver[FP_SAP_LEN];
    uint8_t want[FP_DESCRIPTOR_LEN];
    uint8_t got[FP_DESCRIPTOR_LEN];

    load_hex(checker, "initiator sap", fp_vectors_initiator_sap, initiator, sizeof initiator);
    for (unsigned i = 0; i < fp_vectors_exchange_count; i++) {
        const fp_exchange_vector *v = &fp_vectors_exchange[i];
        load_hex(checker, v->name, v->receiver_sap, receiver, sizeof receiver);

        load_hex(checker, v->name, v->descriptor, want, sizeof want);
        memset(got, 0, sizeof got);
        fp_descriptor(initiator, receiver, got);
        char name[64];
        snprintf(name, sizeof name, "%s descriptor", v->name);
        check_bytes(checker, name, got, want, sizeof want);

        load_hex(checker, v->name, v->exchange, want, sizeof want);
        memset(got, 0, sizeof got);
        fp_exchange(initiator, receiver, got);
        snprintf(name, sizeof name, "%s exchange", v->name);
        check_bytes(checker, name, got, want, sizeof want);
    }
}

static void run_wrapping_key(fp_checker *checker)
{
    uint8_t receiver[FP_SAP_LEN];
    uint8_t m3[FP_M3_LEN];
    uint8_t want[FP_WRAPPING_KEY_LEN];
    uint8_t got[FP_WRAPPING_KEY_LEN];

    load_hex(checker, "receiver sap", fp_vectors_receiver_sap, receiver, sizeof receiver);
    for (unsigned i = 0; i < fp_vectors_message_count; i++) {
        const fp_message_vector *v = &fp_vectors_message[i];
        load_hex(checker, v->name, v->m3, m3, sizeof m3);
        load_hex(checker, v->name, v->wrapping_key, want, sizeof want);
        memset(got, 0, sizeof got);
        check_int(checker, v->name, fp_wrapping_key(receiver, m3, got), FP_OK);
        check_bytes(checker, v->name, got, want, sizeof want);
    }
}

static void run_chain(fp_checker *checker)
{
    for (unsigned i = 0; i < fp_vectors_chain_count; i++) {
        const fp_chain_vector *v = &fp_vectors_chain[i];
        uint8_t entropy[FP_SAP_LEN - 2];
        uint8_t mask[16];
        uint8_t raw_key[FP_RAW_KEY_LEN];
        uint8_t m1[FP_M1_LEN];
        uint8_t m2[FP_M2_LEN];
        uint8_t m3[FP_M3_LEN];
        uint8_t m4[FP_M4_LEN];
        uint8_t ekey[FP_EKEY_LEN];
        uint8_t mac_key[FP_DESCRIPTOR_LEN];
        uint8_t got[FP_M3_LEN];
        scripted_random plan;
        fp_session_t session;
        char name[80];

        memset(&plan, 0, sizeof plan);
        load_hex(checker, v->name, v->entropy, entropy, sizeof entropy);
        load_hex(checker, v->name, v->mask, mask, sizeof mask);
        load_hex(checker, v->name, v->raw_key, raw_key, sizeof raw_key);
        load_hex(checker, v->name, v->m1, m1, sizeof m1);
        load_hex(checker, v->name, v->m2, m2, sizeof m2);
        load_hex(checker, v->name, v->m3, m3, sizeof m3);
        load_hex(checker, v->name, v->m4, m4, sizeof m4);
        load_hex(checker, v->name, v->ekey, ekey, sizeof ekey);
        load_hex(checker, v->name, v->mac_key, mac_key, sizeof mac_key);

        plan.first = entropy;
        plan.first_len = sizeof entropy;
        plan.second = mask;
        plan.second_len = sizeof mask;

        snprintf(name, sizeof name, "%s init", v->name);
        check_int(checker, name, fp_session_init(&session, scripted_random_fn, &plan), FP_OK);
        /* local SAP must be 00 01 || entropy. */
        check_int(checker, name, session.local_sap[0], 0);
        check_int(checker, name, session.local_sap[1], 1);
        check_bytes(checker, name, session.local_sap + 2, entropy, sizeof entropy);

        memset(got, 0, sizeof got);
        fp_message1(&session, got);
        snprintf(name, sizeof name, "%s m1", v->name);
        check_bytes(checker, name, got, m1, sizeof m1);

        memset(got, 0, sizeof got);
        snprintf(name, sizeof name, "%s m3", v->name);
        check_int(checker, name, fp_exchange_m3(&session, m2, sizeof m2, got), FP_OK);
        check_bytes(checker, name, got, m3, sizeof m3);
        /* The m3 tail is also the m4 payload; the session must have kept it. */
        check_bytes(checker, name, session.m3, m3, sizeof m3);
        check_int(checker, name, session.has_m3 ? 1 : 0, 1);

        snprintf(name, sizeof name, "%s m4", v->name);
        check_int(checker, name, fp_confirm_m4(&session, m4, sizeof m4), FP_OK);
        m4[12] ^= 0x01;
        check_int(checker, name, fp_confirm_m4(&session, m4, sizeof m4), FP_ERR_CONFIRM);

        plan.calls = 1; /* the next entropy draw is the ekey mask */
        memset(got, 0, sizeof got);
        snprintf(name, sizeof name, "%s ekey", v->name);
        check_int(checker, name, fp_wrap_key(&session, raw_key, scripted_random_fn, &plan, got), FP_OK);
        check_bytes(checker, name, got, ekey, sizeof ekey);

        /* The MAC key the ekey tag was built with is the descriptor of the SAP
         * pair, not the wrapping key. */
        uint8_t derived_mac[FP_DESCRIPTOR_LEN];
        fp_descriptor(session.local_sap, session.remote_sap, derived_mac);
        snprintf(name, sizeof name, "%s mac key", v->name);
        check_bytes(checker, name, derived_mac, mac_key, sizeof mac_key);
    }
}

static void run_unwrap(fp_checker *checker)
{
    for (unsigned i = 0; i < fp_vectors_unwrap_count; i++) {
        const fp_unwrap_vector *v = &fp_vectors_unwrap[i];
        uint8_t receiver[FP_SAP_LEN];
        uint8_t m3[FP_M3_LEN];
        uint8_t sender_sap[FP_SAP_LEN];
        uint8_t want_key[FP_WRAPPING_KEY_LEN];
        uint8_t raw_key[FP_RAW_KEY_LEN];
        uint8_t mask[16];
        uint8_t ekey[FP_EKEY_LEN];
        uint8_t got[FP_SAP_LEN];
        char name[80];

        load_hex(checker, v->name, v->receiver_sap, receiver, sizeof receiver);
        load_hex(checker, v->name, v->m3, m3, sizeof m3);
        load_hex(checker, v->name, v->sender_sap, sender_sap, sizeof sender_sap);
        load_hex(checker, v->name, v->wrapping_key, want_key, sizeof want_key);
        load_hex(checker, v->name, v->raw_key, raw_key, sizeof raw_key);
        load_hex(checker, v->name, v->ekey, ekey, sizeof ekey);

        memset(got, 0, sizeof got);
        snprintf(name, sizeof name, "%s wrapping key", v->name);
        check_int(checker, name, fp_wrapping_key(receiver, m3, got), FP_OK);
        check_bytes(checker, name, got, want_key, sizeof want_key);

        memset(got, 0, sizeof got);
        snprintf(name, sizeof name, "%s sender sap", v->name);
        check_int(checker, name, fp_message_decrypt(m3[12], m3 + 16, got), FP_OK);
        check_bytes(checker, name, got, sender_sap, sizeof sender_sap);

        uint8_t want_mac[FP_DESCRIPTOR_LEN];
        load_hex(checker, v->name, v->mac_key, want_mac, sizeof want_mac);
        uint8_t got_mac[FP_DESCRIPTOR_LEN];
        fp_descriptor(sender_sap, receiver, got_mac);
        snprintf(name, sizeof name, "%s mac key", v->name);
        check_bytes(checker, name, got_mac, want_mac, sizeof want_mac);

        /* ekey[56:72] must be AES-128(wrapping key, raw key ^ mask). The
         * capture's HMAC tag comes from another implementation, so only the
         * AES block is comparable here. */
        memcpy(mask, ekey + 16, sizeof mask);
        uint8_t masked[FP_RAW_KEY_LEN];
        uint8_t wrapped[16];
        for (int b = 0; b < FP_RAW_KEY_LEN; b++) {
            masked[b] = (uint8_t)(raw_key[b] ^ mask[b]);
        }
        fp_aes128_encrypt_block(want_key, masked, wrapped);
        snprintf(name, sizeof name, "%s ekey block", v->name);
        check_bytes(checker, name, wrapped, ekey + 56, sizeof wrapped);
        snprintf(name, sizeof name, "%s ekey header", v->name);
        check_int(checker, name, fp_rd_be32(ekey + 8), FP_EKEY_LEN - 12);
        check_int(checker, name, fp_rd_be32(ekey + 32), FP_RAW_KEY_LEN);
    }
}

static void run_reject(fp_checker *checker)
{
    const fp_chain_vector *v = &fp_vectors_chain[0];
    uint8_t entropy[FP_SAP_LEN - 2];
    uint8_t mask[16];
    uint8_t raw_key[FP_RAW_KEY_LEN];
    uint8_t m2[FP_M2_LEN];
    uint8_t m4[FP_M4_LEN];
    uint8_t out[FP_M3_LEN];
    scripted_random plan;
    fp_session_t session;

    memset(&plan, 0, sizeof plan);
    load_hex(checker, v->name, v->entropy, entropy, sizeof entropy);
    load_hex(checker, v->name, v->mask, mask, sizeof mask);
    load_hex(checker, v->name, v->raw_key, raw_key, sizeof raw_key);
    load_hex(checker, v->name, v->m2, m2, sizeof m2);
    load_hex(checker, v->name, v->m4, m4, sizeof m4);
    plan.first = entropy;
    plan.first_len = sizeof entropy;
    plan.second = mask;
    plan.second_len = sizeof mask;

    check_int(checker, "init without entropy source", fp_session_init(&session, NULL, NULL), FP_ERR_ARG);
    check_int(checker, "init null session", fp_session_init(NULL, scripted_random_fn, &plan), FP_ERR_ARG);
    check_int(checker, "init", fp_session_init(&session, scripted_random_fn, &plan), FP_OK);

    /* Before m3 exists both follow-up calls must refuse to run. */
    check_int(checker, "m4 before m3", fp_confirm_m4(&session, m4, sizeof m4), FP_ERR_STATE);
    check_int(checker, "wrap before m3", fp_wrap_key(&session, raw_key, scripted_random_fn, &plan, out),
              FP_ERR_STATE);

    check_int(checker, "exchange null m2", fp_exchange_m3(&session, NULL, 0, out), FP_ERR_ARG);
    check_int(checker, "exchange short m2", fp_exchange_m3(&session, m2, sizeof m2 - 1, out), FP_ERR_LENGTH);

    uint8_t bad[FP_M2_LEN];
    memcpy(bad, m2, sizeof bad);
    bad[0] = 'X';
    check_int(checker, "exchange bad magic", fp_exchange_m3(&session, bad, sizeof bad, out), FP_ERR_HEADER);
    memcpy(bad, m2, sizeof bad);
    bad[6] = 3;
    check_int(checker, "exchange bad type", fp_exchange_m3(&session, bad, sizeof bad, out), FP_ERR_HEADER);
    memcpy(bad, m2, sizeof bad);
    bad[12] = 3;
    check_int(checker, "exchange bad marker", fp_exchange_m3(&session, bad, sizeof bad, out), FP_ERR_HEADER);
    memcpy(bad, m2, sizeof bad);
    bad[13] = FP_MODE_COUNT;
    check_int(checker, "exchange bad mode", fp_exchange_m3(&session, bad, sizeof bad, out), FP_ERR_MODE);

    /* A valid m2 must still be rejected after the failures above. */
    check_int(checker, "exchange valid m2", fp_exchange_m3(&session, m2, sizeof m2, out), FP_OK);
    check_int(checker, "m4 short", fp_confirm_m4(&session, m4, sizeof m4 - 1), FP_ERR_LENGTH);
    check_int(checker, "m4 null", fp_confirm_m4(&session, NULL, sizeof m4), FP_ERR_ARG);

    /* Wrapping key input validation: mode lives in m3[12]. */
    uint8_t receiver[FP_SAP_LEN];
    uint8_t m3[FP_M3_LEN];
    uint8_t key[FP_WRAPPING_KEY_LEN];
    load_hex(checker, "receiver sap", fp_vectors_receiver_sap, receiver, sizeof receiver);
    load_hex(checker, "chain m3", v->m3, m3, sizeof m3);
    m3[12] = FP_MODE_COUNT;
    check_int(checker, "wrapping key bad mode", fp_wrapping_key(receiver, m3, key), FP_ERR_MODE);
    check_int(checker, "wrapping key null output", fp_wrapping_key(receiver, m3, NULL), FP_ERR_ARG);
}

int fp_selftest_run(unsigned category_mask, fp_test_stats stats[FP_TEST_CATEGORY_COUNT])
{
    static void (*const runners[FP_TEST_CATEGORY_COUNT])(fp_checker *) = {
        run_platform, run_sap_hash, run_md5, run_message, run_exchange,
        run_wrapping_key, run_chain, run_unwrap, run_reject,
    };

    if (stats == NULL) {
        return FP_ERR_ARG;
    }
    memset(stats, 0, sizeof(fp_test_stats) * FP_TEST_CATEGORY_COUNT);

    unsigned total_failed = 0;
    for (int category = 0; category < FP_TEST_CATEGORY_COUNT; category++) {
        if ((category_mask & (1u << category)) == 0) {
            continue;
        }
        fp_checker checker = {
            .stats = &stats[category],
            .category = (fp_test_category)category,
            .reported = false,
        };
        runners[category](&checker);
        total_failed += stats[category].failed;
    }
    return total_failed == 0 ? FP_OK : -1;
}

int fp_selftest(void)
{
    fp_test_stats stats[FP_TEST_CATEGORY_COUNT];
    unsigned checked = 0;
    unsigned failed = 0;
    const unsigned all_categories = (1u << FP_TEST_CATEGORY_COUNT) - 1u;

    int rc = fp_selftest_run(all_categories, stats);
    for (int category = 0; category < FP_TEST_CATEGORY_COUNT; category++) {
        checked += stats[category].checked;
        failed += stats[category].failed;
    }
    printf("[fairplay] selftest: %u checks, %u failures\n", checked, failed);
    return rc;
}
