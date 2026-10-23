/*
 * Private interface of the FairPlay SAP component.
 *
 * Nothing here is part of the public contract: fairplay.h is the only header
 * an application includes. The split mirrors the Go reference implementation
 * one file at a time so each C translation unit can be reviewed next to its
 * origin:
 *
 *   fp_sap.c            fairplay_sap.go   SAP hash (SAP-specific hash, not MD5)
 *   fp_md5.c            fairplay_md5.go   modified MD5 compression + mutations
 *   fp_message.c        fairplay_message.go  SAS-CBC message cipher + AES rounds
 *   fpsap.c             fpsap.go          white-box networks, descriptor,
 *                                          exchange, m1/m3/m4, key wrapping
 *   fpsap_tables.c      fpsap_tables.go   substitution/mix bases + lookup triples
 *   fp_platform_esp.c   (platform)        SHA-512 and HMAC-SHA1 via PSA
 *   fp_selftest.c       tests             re-derives every published vector
 *   fp_vectors.c        (generated)       vector data
 *
 * Generated data files (do not edit by hand):
 *   fp_data_sap.inc, fp_data_md5.inc, fp_data_message.inc, fp_data_fpsap.inc
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fairplay.h"

#define FP_MODE_COUNT 4
#define FP_BODY_LEN 128
#define FP_DESCRIPTOR_LEN 20
#define FP_WRAPPING_KEY_LEN 16
#define FP_SHA1_LEN 20
#define FP_SHA512_LEN 64
/* The pair secret is an X25519 shared secret (32 bytes); 64 leaves room. */
#define FP_PAIR_SECRET_MAX 64

/* Return codes. The public API only promises 0 == success, so these values are
 * an internal detail; they make fp_selftest output diagnosable. */
enum {
    FP_OK = 0,
    FP_ERR_ARG = -1,      /* NULL or otherwise unusable argument */
    FP_ERR_LENGTH = -2,   /* wrong record/body length */
    FP_ERR_HEADER = -3,   /* FPLY magic, version or declared length mismatch */
    FP_ERR_MODE = -4,     /* mode outside 0..3 */
    FP_ERR_STATE = -5,    /* m3 was not produced yet */
    FP_ERR_CONFIRM = -6,  /* m4 does not echo m3[144:164] */
    FP_ERR_CRYPTO = -7,   /* platform primitive (PSA) failed */
    FP_ERR_RANDOM = -8,   /* entropy callback failed */
};

/* ------------------------------------------------------------------------- */
/* byte order helpers                                                         */
/*                                                                            */
/* The protocol mixes both orders on purpose: FPLY length fields are big       */
/* endian, MD5 message padding is little endian, the modified MD5 reads its    */
/* message words big endian but is seeded with little-endian words. Keeping   */
/* the helper name explicit is the cheapest way to stay honest about it.      */
/* ------------------------------------------------------------------------- */

static inline uint32_t fp_rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint32_t fp_rd_le32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

static inline void fp_wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void fp_wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void fp_wr_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static inline uint8_t fp_rotl8(uint8_t v, unsigned count)
{
    count &= 7u;
    return count ? (uint8_t)((v << count) | (v >> (8 - count))) : v;
}

/* ------------------------------------------------------------------------- */
/* fp_sap.c                                                                   */
/* ------------------------------------------------------------------------- */

/* FairPlay's SAP hash: 840-round scramble, straight-line nonlinear circuit,
 * 256-step reverse diffusion. Not a standard hash, no AES involved. */
void fp_sap_hash(const uint8_t block[64], uint8_t out[16]);

/* ------------------------------------------------------------------------- */
/* fp_md5.c                                                                   */
/* ------------------------------------------------------------------------- */

typedef enum {
    FP_MUTATION_SWAP = 0,  /* fpsapSwapMutation */
    FP_MUTATION_CYCLE = 1, /* fpsapCycleMutation */
    FP_MUTATION_KDF = 2,   /* fairplayKDFMutation */
} fp_mutation;

/* One MD5 compression round with FairPlay's two deviations from standard MD5:
 * message words are read big endian, and the schedule is mutated right after
 * round 31 using the post-update a/b/c/d. Only the compression is needed. */
void fp_md5_compress(uint32_t state[4], const uint8_t block[64], fp_mutation mutation);

/* ------------------------------------------------------------------------- */
/* fp_message.c                                                               */
/* ------------------------------------------------------------------------- */

/* SAS-CBC over exactly 128 bytes, 8 blocks, no padding. `mode` selects one of
 * four fixed IVs and one of four sets of nine middle round keys. `out` must not
 * alias the input: the CBC chain reads back from the output buffer. */
int fp_message_encrypt(uint8_t mode, const uint8_t plain[FP_BODY_LEN], uint8_t out[FP_BODY_LEN]);
int fp_message_decrypt(uint8_t mode, const uint8_t cipher[FP_BODY_LEN], uint8_t out[FP_BODY_LEN]);

/* Standard AES-128 single block encryption, used only for ekey[56:72]. Built on
 * the round primitives above so the component needs no AES implementation from
 * outside (the SAS-CBC round keys cannot come from a normal key schedule). */
void fp_aes128_encrypt_block(const uint8_t key[FP_WRAPPING_KEY_LEN], const uint8_t in[16], uint8_t out[16]);

/* ------------------------------------------------------------------------- */
/* fpsap_tables.c (generated from the Go tables)                              */
/*                                                                            */
/* The Go tables store 13 permutation bases plus (base, in_xor, out_xor)      */
/* triples instead of 176 expanded tables. Same layout here: 3 bytes per      */
/* lookup keeps the two networks' 352 entries at 1056 bytes.                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t table;
    uint8_t in_xor;
    uint8_t out_xor;
} fp_lookup;

extern const uint8_t fpsap_first_input_mask[16];
extern const uint8_t fpsap_second_output_mask[16];
extern const uint8_t fpsap_substitution_bases[6][256];
extern const uint8_t fpsap_mix_bases[7][256];
extern const fp_lookup fpsap_first_round[9][16];
extern const fp_lookup fpsap_first_mix[4][4];
extern const fp_lookup fpsap_first_final[16];
extern const fp_lookup fpsap_second_round[9][16];
extern const fp_lookup fpsap_second_mix[4][4];
extern const fp_lookup fpsap_second_final[16];

/* ------------------------------------------------------------------------- */
/* fpsap.c                                                                    */
/* ------------------------------------------------------------------------- */

/* 20-byte white-box seed from both halves of an exchange. The initiator SAP
 * comes first: descriptor(sender, receiver). */
void fp_descriptor(const uint8_t sender_sap[FP_SAP_LEN], const uint8_t receiver_sap[FP_SAP_LEN],
                   uint8_t out[FP_DESCRIPTOR_LEN]);

/* descriptor + masks + two white-box networks + two digest32 rounds. The 20-byte
 * result is the m3 tail and, for the receiver, its m4 confirmation value. */
void fp_exchange(const uint8_t sender_sap[FP_SAP_LEN], const uint8_t receiver_sap[FP_SAP_LEN],
                 uint8_t out[FP_DESCRIPTOR_LEN]);

/* AES-128 key for ekey[56:72]; decrypts the m3 body and mixes it with the
 * receiver SAP. Block order here is compress-then-add-hash, the opposite of
 * fp_descriptor's add-hash-then-compress. */
int fp_wrapping_key(const uint8_t receiver_sap[FP_SAP_LEN], const uint8_t m3[FP_M3_LEN],
                    uint8_t out[FP_WRAPPING_KEY_LEN]);

/* FPLY record helpers: magic "FPLY", {3,1,type,0}, big-endian payload length. */
int fp_validate_record(const uint8_t *record, size_t len, uint8_t type, size_t payload_len);
void fp_build_record(uint8_t type, size_t payload_len, uint8_t *out);

/* ------------------------------------------------------------------------- */
/* fp_platform_*.c                                                           */
/* ------------------------------------------------------------------------- */

/* One-shot SHA-512 (stream key mixing) and HMAC-SHA1 (ekey tag). Returns 0 on
 * success, FP_ERR_CRYPTO otherwise. */
int fp_sha512(const uint8_t *data, size_t len, uint8_t out[FP_SHA512_LEN]);
int fp_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                 uint8_t out[FP_SHA1_LEN]);

/* ------------------------------------------------------------------------- */
/* fp_vectors.c (generated from the Go reference implementation)              */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *block;  /* 64 bytes hex */
    const char *digest; /* 16 bytes hex */
} fp_hash_vector;

typedef struct {
    const char *name;
    fp_mutation mutation;
    const char *block;
    const char *digest;
} fp_md5_vector;

typedef struct {
    const char *name;
    uint8_t mode;
    const char *cipher;       /* 128-byte body of the synthetic m3 */
    const char *plain;        /* decryption result */
    const char *m3;           /* 164-byte synthetic message (mode at [12]) */
    const char *wrapping_key; /* fp_wrapping_key(receiver_sap, m3) */
} fp_message_vector;

typedef struct {
    const char *name;
    const char *receiver_sap; /* 128 bytes hex */
    const char *descriptor;   /* 20 bytes hex */
    const char *exchange;     /* 20 bytes hex */
} fp_exchange_vector;

typedef struct {
    const char *name;
    const char *entropy;  /* 126 bytes for fp_session_init */
    const char *m1;       /* 16 bytes */
    const char *m2;       /* 142 bytes */
    const char *m3;       /* 164 bytes */
    const char *m4;       /* 32 bytes */
    const char *raw_key;  /* 16 bytes */
    const char *mask;     /* 16 bytes, the second entropy draw */
    const char *ekey;     /* 72 bytes */
    const char *mac_key;  /* 20 bytes, descriptor(senderSAP, receiverSAP) */
} fp_chain_vector;

typedef struct {
    const char *name;
    const char *receiver_sap;
    const char *m3;
    const char *sender_sap;   /* recovered from the m3 body */
    const char *wrapping_key;
    const char *mac_key;
    const char *raw_key;      /* unwraps ekey[56:72]; the capture's tag is not
                               * reproducible, local HMAC is tested via chains */
    const char *ekey;
} fp_unwrap_vector;

typedef struct {
    const char *name;
    const char *key;
    const char *msg;
    const char *digest;
} fp_mac_vector;

typedef struct {
    const char *name;
    const char *data;
    const char *digest;
} fp_sha512_vector;

typedef struct {
    const char *name;
    const char *raw_key;
    const char *pair_secret;
    const char *stream_key;
} fp_stream_key_vector;

extern const char fp_vectors_initiator_sap[]; /* reference sender SAP, 128 bytes hex */
extern const char fp_vectors_receiver_sap[];  /* reference receiver SAP, 128 bytes hex */

extern const fp_hash_vector fp_vectors_sap_hash[];
extern const unsigned fp_vectors_sap_hash_count;
extern const fp_md5_vector fp_vectors_md5[];
extern const unsigned fp_vectors_md5_count;
extern const fp_message_vector fp_vectors_message[];
extern const unsigned fp_vectors_message_count;
extern const fp_exchange_vector fp_vectors_exchange[];
extern const unsigned fp_vectors_exchange_count;
extern const fp_chain_vector fp_vectors_chain[];
extern const unsigned fp_vectors_chain_count;
extern const fp_unwrap_vector fp_vectors_unwrap[];
extern const unsigned fp_vectors_unwrap_count;
extern const fp_mac_vector fp_vectors_hmac_sha1[];
extern const unsigned fp_vectors_hmac_sha1_count;
extern const fp_sha512_vector fp_vectors_sha512[];
extern const unsigned fp_vectors_sha512_count;
extern const fp_stream_key_vector fp_vectors_stream_key[];
extern const unsigned fp_vectors_stream_key_count;

/* ------------------------------------------------------------------------- */
/* fp_selftest.c                                                             */
/* ------------------------------------------------------------------------- */

typedef enum {
    FP_TEST_PLATFORM = 0,  /* SHA-512 / HMAC-SHA1 known answers, stream key */
    FP_TEST_SAP_HASH,      /* 64 corpus blocks + the deterministic block */
    FP_TEST_MD5,           /* swap / cycle / kdf mutations */
    FP_TEST_MESSAGE,       /* mode 0..3 encrypt + decrypt */
    FP_TEST_EXCHANGE,      /* descriptor + 20-byte exchange per case */
    FP_TEST_WRAPPING_KEY,  /* per-mode wrapping keys */
    FP_TEST_CHAIN,         /* m1 -> m3 -> m4 -> ekey, incl. HMAC and AES block */
    FP_TEST_UNWRAP,        /* captured m3/ekey unwrap */
    FP_TEST_REJECT,        /* malformed m2/m4 must be refused */
    FP_TEST_CATEGORY_COUNT
} fp_test_category;

typedef struct {
    unsigned checked;
    unsigned failed;
} fp_test_stats;

/* Runs the requested categories (bit i == FP_TEST_* i) and fills per-category
 * counters. The first failing check of each category is printed with got/want. */
int fp_selftest_run(unsigned category_mask, fp_test_stats stats[FP_TEST_CATEGORY_COUNT]);

const char *fp_test_category_name(fp_test_category category);
