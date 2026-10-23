/*
 * FairPlay SAP exchange: white-box networks, descriptor, FPLY records and the
 * ekey wrapping (ports of fpsap.go and fairplay_crypto.go).
 *
 * The derivation chain, with the initiator's SAP (sender) first everywhere:
 *
 *   descriptor(sender, receiver)           20 bytes of white-box seed
 *     -> masks(seed)                       9 x 16 round masks
 *     -> first_network                     intermediate
 *     -> digest32(intermediate, fixed)     left
 *     -> second_network(left, masks)       white
 *     -> digest32(left, white)             digest
 *   exchange = white[0:4] || digest        20 bytes, the m3 tail / m4 body
 *
 *   m3 body   = SAS-CBC(mode, IV[mode], sender SAP)
 *   m4        = 12-byte FPLY header + m3[144:164]
 *   wrap key  = KDF(receiver SAP, m3)      AES-128 key for ekey[56:72]
 *   mac key   = descriptor(sender SAP, receiver SAP)
 *   ekey      = header || mask || 16 || HMAC-SHA1(mac key) || AES(wrap key)
 */
#include <string.h>

#include "fp_internal.h"

#include "fp_data_fpsap.inc"

/* ------------------------------------------------------------------------- */
/* white-box networks                                                         */
/* ------------------------------------------------------------------------- */

/* A lookup triple is (base table, input xor, output xor) into one of the 13
 * permutation bases; expansion happens on the fly. */
static uint8_t lookup_substitute(const fp_lookup *lookup, uint8_t value)
{
    return (uint8_t)(fpsap_substitution_bases[lookup->table][value ^ lookup->in_xor] ^ lookup->out_xor);
}

static uint8_t lookup_mix(const fp_lookup *lookup, uint8_t value)
{
    return (uint8_t)(fpsap_mix_bases[lookup->table][value ^ lookup->in_xor] ^ lookup->out_xor);
}

/* One 4x4 mixing layer per 4-byte word. The table is indexed [input byte]
 * [output byte] and the input is the substituted block, not the state. */
static void mix_layer(const fp_lookup mix[4][4], uint8_t state[16], const uint8_t substituted[16])
{
    for (int word = 0; word < 4; word++) {
        int offset = word * 4;
        for (int out_byte = 0; out_byte < 4; out_byte++) {
            uint8_t mixed = 0;
            for (int in_byte = 0; in_byte < 4; in_byte++) {
                mixed ^= lookup_mix(&mix[in_byte][out_byte], substituted[offset + in_byte]);
            }
            state[offset + out_byte] = mixed;
        }
    }
}

/* First network, banks ascending. Note the two asymmetries against the second
 * network: the table is indexed with map[o] (the state index) and the mask is
 * xored over the whole block after the mixing layer. */
static void first_network(const uint8_t masks[9][16], uint8_t out[16])
{
    uint8_t state[16];
    for (int i = 0; i < 16; i++) {
        state[i] = (uint8_t)(fpsap_fixed_block[i] ^ fpsap_first_input_mask[i]);
    }
    for (int bank = 0; bank < 9; bank++) {
        uint8_t substituted[16];
        for (int output = 0; output < 16; output++) {
            int input = fpsap_first_position_map[output];
            substituted[output] = lookup_substitute(&fpsap_first_round[bank][input], state[input]);
        }
        mix_layer(fpsap_first_mix, state, substituted);
        for (int i = 0; i < 16; i++) {
            state[i] ^= masks[bank][i];
        }
    }
    for (int output = 0; output < 16; output++) {
        int input = fpsap_first_position_map[output];
        out[output] = lookup_substitute(&fpsap_first_final[input], state[input]);
    }
}

/* Second network, banks descending. Here the table is indexed with the output
 * position and the mask is xored per byte right after the substitution; the
 * final lookup also xors the second output mask (the first network does not
 * have such a mask). */
static void second_network(const uint8_t in[16], const uint8_t masks[9][16], uint8_t out[16])
{
    uint8_t state[16];
    memcpy(state, in, 16);
    for (int bank = 8; bank >= 0; bank--) {
        uint8_t substituted[16];
        for (int output = 0; output < 16; output++) {
            int input = fpsap_second_position_map[output];
            substituted[output] =
                (uint8_t)(lookup_substitute(&fpsap_second_round[bank][output], state[input]) ^ masks[bank][output]);
        }
        mix_layer(fpsap_second_mix, state, substituted);
    }
    for (int output = 0; output < 16; output++) {
        int input = fpsap_second_position_map[output];
        out[output] = (uint8_t)(lookup_substitute(&fpsap_second_final[output], state[input]) ^ fpsap_second_output_mask[output]);
    }
}

/* One modified-MD5 compression over (left || right || 0x80 || 256 bits); the
 * 256 is the logical payload length in bits, written little endian. */
static void digest32(const uint8_t left[16], const uint8_t right[16], uint8_t out[16])
{
    uint8_t block[64];
    uint32_t state[4] = {0xb9f3dcdc, 0xfbdc740b, 0x60f77f86, 0x51907216};

    memset(block, 0, sizeof block);
    memcpy(block, left, 16);
    memcpy(block + 16, right, 16);
    block[32] = 0x80;
    fp_wr_le32(block + 56, 0x100);
    fp_md5_compress(state, block, FP_MUTATION_SWAP);
    for (int word = 0; word < 4; word++) {
        fp_wr_be32(out + word * 4, state[word]);
    }
}

/* Nine round masks. Every iteration restarts from the same hard-coded state, so
 * the masks are independent of each other. */
static void masks_from_seed(const uint8_t seed[FP_DESCRIPTOR_LEN], uint8_t masks[9][16])
{
    static const uint8_t suffix[15] = {
        0x57, 0xd8, 0xee, 0xcb, 0xde, 0xfb, 0xcf, 0x59,
        0x1c, 0x27, 0xa2, 0xcf, 0xbe, 0xb0, 0x89,
    };
    const uint32_t initial[4] = {0x1d4a4587, 0x92f39fcc, 0x1d87d836, 0xcdc86697};

    for (int i = 0; i < 9; i++) {
        uint8_t block[64];
        uint32_t state[4];
        memset(block, 0, sizeof block);
        memcpy(block, seed, FP_DESCRIPTOR_LEN);
        block[20] = (uint8_t)i;
        memcpy(block + 21, suffix, sizeof suffix);
        block[36] = 0x80;
        /* Only four bytes are written: block[60..63] stay zero, so this is not
         * a full 64-bit length field. */
        fp_wr_le32(block + 56, 0x320);
        memcpy(state, initial, sizeof state);
        fp_md5_compress(state, block, FP_MUTATION_SWAP);
        for (int word = 0; word < 4; word++) {
            fp_wr_be32(masks[i] + word * 4, state[word]);
        }
    }
}

void fp_descriptor(const uint8_t sender_sap[FP_SAP_LEN], const uint8_t receiver_sap[FP_SAP_LEN],
                   uint8_t out[FP_DESCRIPTOR_LEN])
{
    uint8_t padded[320];
    uint32_t state[4];
    uint32_t last_first[4];
    int offset = 0;

    memset(padded, 0, sizeof padded);
    memcpy(padded + offset, fpsap_descriptor_prefix, 17);
    offset += 17;
    memcpy(padded + offset, sender_sap, FP_SAP_LEN);
    offset += FP_SAP_LEN;
    memcpy(padded + offset, receiver_sap, FP_SAP_LEN);
    offset += FP_SAP_LEN;
    memcpy(padded + offset, fpsap_descriptor_suffix, 17);
    offset += 17;
    padded[offset] = 0x80;                            /* offset == 290, padding marker */
    fp_wr_le64(padded + 312, (uint64_t)offset * 8);   /* 2320 bits, little endian */

    /* The state starts from little-endian words of the fixed session key. */
    for (int word = 0; word < 4; word++) {
        state[word] = fp_rd_le32(fairplay_initial_session_key + word * 4);
    }

    for (int block = 0; block < 320; block += 64) {
        uint8_t add[16];
        /* Order matters: fold the SAP hash of the block into the state first,
         * then compress with that already-updated state. */
        fp_sap_hash(padded + block, add);
        for (int word = 0; word < 4; word++) {
            state[word] += fp_rd_le32(add + word * 4);
        }
        fp_md5_compress(state, padded + block, FP_MUTATION_CYCLE);
        if (block == 256) {
            /* Last block is compressed twice; only the first result's first
             * word survives into the output. */
            memcpy(last_first, state, sizeof last_first);
            fp_md5_compress(state, padded + block, FP_MUTATION_CYCLE);
        }
    }

    fp_wr_be32(out, last_first[0]);
    for (int word = 0; word < 4; word++) {
        fp_wr_be32(out + 4 + word * 4, state[word]);
    }
}

static void exchange_seed(const uint8_t seed[FP_DESCRIPTOR_LEN], uint8_t out[FP_DESCRIPTOR_LEN])
{
    uint8_t masks[9][16];
    uint8_t intermediate[16];
    uint8_t left[16];
    uint8_t white[16];
    uint8_t digest[16];

    masks_from_seed(seed, masks);
    first_network(masks, intermediate);
    digest32(intermediate, fpsap_fixed_block, left);
    second_network(left, masks, white);
    digest32(left, white, digest);

    memcpy(out, white, 4);
    memcpy(out + 4, digest, 16);
}

void fp_exchange(const uint8_t sender_sap[FP_SAP_LEN], const uint8_t receiver_sap[FP_SAP_LEN],
                 uint8_t out[FP_DESCRIPTOR_LEN])
{
    uint8_t seed[FP_DESCRIPTOR_LEN];
    fp_descriptor(sender_sap, receiver_sap, seed);
    exchange_seed(seed, out);
}

int fp_wrapping_key(const uint8_t receiver_sap[FP_SAP_LEN], const uint8_t m3[FP_M3_LEN],
                    uint8_t out[FP_WRAPPING_KEY_LEN])
{
    uint8_t decrypted[FP_BODY_LEN];
    uint8_t material[320];
    uint32_t state[4];
    int offset = 0;
    int mode;

    if (receiver_sap == NULL || m3 == NULL || out == NULL) {
        return FP_ERR_ARG;
    }
    mode = m3[12];
    if (mode >= FP_MODE_COUNT) {
        return FP_ERR_MODE;
    }
    if (fp_message_decrypt((uint8_t)mode, m3 + 16, decrypted) != FP_OK) {
        return FP_ERR_MODE;
    }

    memset(material, 0, sizeof material);
    memcpy(material + offset, fairplay_kdf_prefix, 17);
    offset += 17;
    memcpy(material + offset, decrypted, FP_BODY_LEN);
    offset += FP_BODY_LEN;
    memcpy(material + offset, receiver_sap, FP_SAP_LEN);
    offset += FP_SAP_LEN;
    memcpy(material + offset, fairplay_kdf_suffix, 17);
    offset += 17;
    material[offset] = 0x80;                         /* offset == 290 */
    fp_wr_le64(material + 312, (uint64_t)offset * 8); /* 2320 bits */

    for (int word = 0; word < 4; word++) {
        state[word] = fp_rd_le32(fairplay_initial_session_key + word * 4);
    }

    for (int block = 0; block < 320; block += 64) {
        uint8_t hashed[16];
        uint32_t compressed[4];
        memcpy(compressed, state, sizeof compressed);
        /* The opposite composition from fp_descriptor: compress first, then
         * replace the state with compression + SAP hash of the same block. */
        fp_md5_compress(compressed, material + block, FP_MUTATION_KDF);
        fp_sap_hash(material + block, hashed);
        for (int word = 0; word < 4; word++) {
            state[word] = compressed[word] + fp_rd_le32(hashed + word * 4);
        }
    }

    for (int word = 0; word < 4; word++) {
        fp_wr_be32(out + word * 4, state[word]);
    }
    return FP_OK;
}

/* ------------------------------------------------------------------------- */
/* FPLY records                                                               */
/* ------------------------------------------------------------------------- */

void fp_build_record(uint8_t type, size_t payload_len, uint8_t *out)
{
    memcpy(out, "FPLY", 4);
    out[4] = 3;
    out[5] = 1;
    out[6] = type;
    out[7] = 0;
    fp_wr_be32(out + 8, (uint32_t)payload_len);
}

int fp_validate_record(const uint8_t *record, size_t len, uint8_t type, size_t payload_len)
{
    if (record == NULL) {
        return FP_ERR_ARG;
    }
    if (len != 12 + payload_len) {
        return FP_ERR_LENGTH;
    }
    if (memcmp(record, "FPLY", 4) != 0) {
        return FP_ERR_HEADER;
    }
    if (record[4] != 3 || record[5] != 1 || record[6] != type || record[7] != 0) {
        return FP_ERR_HEADER;
    }
    if (fp_rd_be32(record + 8) != payload_len) {
        return FP_ERR_HEADER;
    }
    return FP_OK;
}

/* ------------------------------------------------------------------------- */
/* session                                                                    */
/* ------------------------------------------------------------------------- */

int fp_session_init(fp_session_t *session, fp_random_fn random, void *ctx)
{
    if (session == NULL || random == NULL) {
        return FP_ERR_ARG;
    }
    memset(session, 0, sizeof *session);
    /* localSAP is 00 01 followed by 126 opaque bytes; byte 0 stays zero. */
    session->local_sap[1] = 1;
    if (random(ctx, session->local_sap + 2, FP_SAP_LEN - 2) != 0) {
        memset(session, 0, sizeof *session);
        return FP_ERR_RANDOM;
    }
    return FP_OK;
}

void fp_message1(const fp_session_t *session, uint8_t out[FP_M1_LEN])
{
    /* m1 is a constant: the 4-byte payload describes capabilities, not the
     * session. The session parameter keeps the call site symmetric with the
     * rest of the handshake. */
    (void)session;
    fp_build_record(1, sizeof fpsap_m1_payload, out);
    memcpy(out + 12, fpsap_m1_payload, sizeof fpsap_m1_payload);
}

int fp_exchange_m3(fp_session_t *session, const uint8_t *m2, size_t m2_len, uint8_t out[FP_M3_LEN])
{
    uint8_t m3[FP_M3_LEN];
    uint8_t remote_sap[FP_SAP_LEN];
    uint8_t mode;

    if (session == NULL || m2 == NULL || out == NULL) {
        return FP_ERR_ARG;
    }
    int rc = fp_validate_record(m2, m2_len, 2, 130);
    if (rc != FP_OK) {
        return rc;
    }
    /* m2[12] is a payload marker that must be 2; m2[13] is the mode the
     * receiver chose; m2[14:142] is its SAP, encrypted with that mode. */
    if (m2[12] != 2) {
        return FP_ERR_HEADER;
    }
    mode = m2[13];
    if (mode >= FP_MODE_COUNT) {
        return FP_ERR_MODE;
    }

    fp_build_record(3, 152, m3);
    m3[12] = mode;
    memcpy(m3 + 13, fpsap_m3_label, sizeof fpsap_m3_label);
    if (fp_message_encrypt(mode, session->local_sap, m3 + 16) != FP_OK) {
        return FP_ERR_MODE;
    }
    /* Decrypting the receiver's SAP uses the same mode, with the body passed
     * directly: the "message[12] = mode" trick of the Go helper only exists so
     * the block cipher can read the mode from the record. */
    if (fp_message_decrypt(mode, m2 + 14, remote_sap) != FP_OK) {
        return FP_ERR_MODE;
    }
    fp_exchange(session->local_sap, remote_sap, m3 + 144);

    memcpy(session->remote_sap, remote_sap, FP_SAP_LEN);
    memcpy(session->m3, m3, FP_M3_LEN);
    session->mode = mode;
    session->has_m3 = true;
    memcpy(out, m3, FP_M3_LEN);
    return FP_OK;
}

int fp_confirm_m4(const fp_session_t *session, const uint8_t *m4, size_t m4_len)
{
    if (session == NULL || m4 == NULL) {
        return FP_ERR_ARG;
    }
    if (!session->has_m3) {
        return FP_ERR_STATE;
    }
    int rc = fp_validate_record(m4, m4_len, 4, 20);
    if (rc != FP_OK) {
        return rc;
    }
    if (memcmp(m4 + 12, session->m3 + 144, 20) != 0) {
        return FP_ERR_CONFIRM;
    }
    return FP_OK;
}

int fp_wrap_key(fp_session_t *session, const uint8_t raw_key[FP_RAW_KEY_LEN],
                fp_random_fn random, void *ctx, uint8_t out[FP_EKEY_LEN])
{
    uint8_t ekey[FP_EKEY_LEN];
    uint8_t wrapping_key[FP_WRAPPING_KEY_LEN];
    uint8_t sender_sap[FP_SAP_LEN];
    uint8_t mac_key[FP_DESCRIPTOR_LEN];
    uint8_t masked[FP_RAW_KEY_LEN];
    uint8_t mac_input[36 + FP_RAW_KEY_LEN];
    uint8_t tag[FP_SHA1_LEN];
    int mode;

    if (session == NULL || raw_key == NULL || out == NULL || random == NULL) {
        return FP_ERR_ARG;
    }
    if (!session->has_m3) {
        return FP_ERR_STATE;
    }
    int rc = fp_validate_record(session->m3, FP_M3_LEN, 3, 152);
    if (rc != FP_OK) {
        return rc;
    }
    mode = session->m3[12];
    if (mode >= FP_MODE_COUNT) {
        return FP_ERR_MODE;
    }

    /* Fixed 16-byte encrypted-key header: FPLY, version 1.2, type 1, then the
     * declared payload length 0x3c = 72 - 12. */
    memcpy(ekey, "FPLY", 4);
    ekey[4] = 0x01;
    ekey[5] = 0x02;
    ekey[6] = 0x01;
    ekey[7] = 0x00;
    fp_wr_be32(ekey + 8, (uint32_t)(FP_EKEY_LEN - 12));
    memset(ekey + 12, 0, 4);

    if (random(ctx, ekey + 16, 16) != 0) {
        return FP_ERR_RANDOM;
    }
    fp_wr_be32(ekey + 32, FP_RAW_KEY_LEN);

    if (fp_wrapping_key(session->remote_sap, session->m3, wrapping_key) != FP_OK) {
        return FP_ERR_MODE;
    }
    for (int i = 0; i < FP_RAW_KEY_LEN; i++) {
        masked[i] = (uint8_t)(raw_key[i] ^ ekey[16 + i]);
    }
    fp_aes128_encrypt_block(wrapping_key, masked, ekey + 56);

    /* The MAC key is the descriptor of the SAP pair, not the wrapping key: it
     * starts from the sender SAP recovered out of the m3 body. */
    if (fp_message_decrypt((uint8_t)mode, session->m3 + 16, sender_sap) != FP_OK) {
        return FP_ERR_MODE;
    }
    fp_descriptor(sender_sap, session->remote_sap, mac_key);

    memcpy(mac_input, ekey, 36);
    memcpy(mac_input + 36, raw_key, FP_RAW_KEY_LEN);
    if (fp_hmac_sha1(mac_key, sizeof mac_key, mac_input, sizeof mac_input, tag) != 0) {
        return FP_ERR_CRYPTO;
    }
    memcpy(ekey + 36, tag, FP_SHA1_LEN);

    memcpy(out, ekey, FP_EKEY_LEN);
    return FP_OK;
}

void fp_stream_key(const uint8_t raw_key[FP_RAW_KEY_LEN], const uint8_t *pair_secret,
                   size_t pair_secret_len, bool mix_pair_secret, uint8_t out[FP_RAW_KEY_LEN])
{
    uint8_t material[FP_RAW_KEY_LEN + FP_PAIR_SECRET_MAX];
    uint8_t digest[FP_SHA512_LEN];

    if (raw_key == NULL || out == NULL) {
        return;
    }
    if (!mix_pair_secret || pair_secret_len == 0 || pair_secret_len > FP_PAIR_SECRET_MAX) {
        /* Legacy path (and any unusable secret): the bare key is the stream key. */
        memcpy(out, raw_key, FP_RAW_KEY_LEN);
        return;
    }
    memcpy(material, raw_key, FP_RAW_KEY_LEN);
    memcpy(material + FP_RAW_KEY_LEN, pair_secret, pair_secret_len);
    if (fp_sha512(material, FP_RAW_KEY_LEN + pair_secret_len, digest) != 0) {
        /* Without SHA-512 the mixed key cannot be derived; falling back to the
         * raw key keeps audio streaming instead of aborting the session. */
        memcpy(out, raw_key, FP_RAW_KEY_LEN);
        return;
    }
    memcpy(out, digest, FP_RAW_KEY_LEN);
}
