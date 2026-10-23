/*
 * Legacy AirTunes checks: the key derivations against golden vectors
 * generated from the real RPiPlay sources (see docs/legacy-spec.md), the
 * continuous CTR keystream semantics, the legacy frame header, and the two
 * legacy SETUP plists.
 */
#include <stdio.h>
#include <string.h>

#include "ap_frame.h"
#include "ap_legacy.h"
#include "ap_plist.h"

static int failures;

static void expect(int condition, const char *what)
{
    if (!condition) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static void unhex(const char *hex, uint8_t *out, size_t cap)
{
    size_t nibbles = strlen(hex);
    for (size_t i = 0; i + 1 < nibbles && i / 2 < cap; i += 2) {
        unsigned byte = 0;
        for (int half = 0; half < 2; half++) {
            char c = hex[i + half];
            byte <<= 4;
            if (c >= '0' && c <= '9') {
                byte |= (unsigned)(c - '0');
            } else {
                byte |= (unsigned)(c - 'a' + 10);
            }
        }
        out[i / 2] = (uint8_t)byte;
    }
}

static void expect_hex(const uint8_t *got, const char *want_hex, const char *what)
{
    uint8_t want[128];
    size_t want_len = strlen(want_hex) / 2;
    unhex(want_hex, want, sizeof(want));
    if (want_len > sizeof(want) || memcmp(got, want, want_len) != 0) {
        failures++;
        printf("FAIL %s\n", what);
        printf("  want %s\n", want_hex);
        printf("  got  ");
        for (size_t i = 0; i < want_len; i++) {
            printf("%02x", got[i]);
        }
        printf("\n");
    }
}

int main(void)
{
    /* Inputs shared with the vector generator: /tmp/legacy_vectors/gen_vectors.c,
     * built from the unmodified RPiPlay crypto (docs/legacy-spec.md). */
    static const char *AESKEY_HEX = "00112233445566778899aabbccddeeff";
    static const char *ECDH_HEX =
        "5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a";
    static const char *SKEY_HEX = "4d50029832059d123a616e9d29704398";
    static const char *SIV_HEX = "cd416cf99a36cd947dc90f0926178aeb";
    static const char *PV_KEY_HEX = "da45e8cf38a27dcf638283777bf11026";
    static const char *PV_IV_HEX = "9d667eb390c58d007c175a1110d2f5d1";
    static const char *CTR96_HEX =
        "915a4c881516a227a3a4ae4de6e8dea345feda89777265de0202b5b95a5c4c7d2"
        "2ff374c522bf48d27e19f3d7a044f95b35142e11155136fff414611545dc547"
        "e18644027d7ad05a98484c6e8ea68dc0850a9331215be366695e66100171d6bd";
    static const char *CTR96_SKIP_HEX =
        "55eeca99676275ce1212a5a94a4c5c6d12cf077c621bc4bd17d1af0d4a347fa5"
        "a34152f10145037fef515601444dd55791f634720d0aa02ae8383c1efed6fdb0"
        "951a8321314bf376794e76001161c6ad2226c68248248252ffccc50a30e6639c";

    uint8_t aeskey[16];
    uint8_t ecdh[32];
    unhex(AESKEY_HEX, aeskey, sizeof(aeskey));
    unhex(ECDH_HEX, ecdh, sizeof(ecdh));

    /* 1. The skey/siv derivation (stream key mixed with the pair secret,
     *    then the labelled second round). */
    uint8_t skey[16];
    uint8_t siv[16];
    ap_legacy_derive_stream_keys(aeskey, ecdh, 1234567890, skey, siv);
    expect_hex(skey, SKEY_HEX, "skey derivation");
    expect_hex(siv, SIV_HEX, "siv derivation");

    /* 2. The pair-verify signature keys. */
    uint8_t digest[64];
    uint8_t material[19 + 32];
    uint8_t pv_key[16];
    uint8_t pv_iv[16];
    memcpy(material, "Pair-Verify-AES-Key", 19);
    memcpy(material + 19, ecdh, 32);
    hap_sha512(material, sizeof(material), digest);
    memcpy(pv_key, digest, 16);
    expect_hex(pv_key, PV_KEY_HEX, "pair-verify key derivation");
    uint8_t iv_material[18 + 32];
    memcpy(iv_material, "Pair-Verify-AES-IV", 18);
    memcpy(iv_material + 18, ecdh, 32);
    hap_sha512(iv_material, sizeof(iv_material), digest);
    memcpy(pv_iv, digest, 16);
    expect_hex(pv_iv, PV_IV_HEX, "pair-verify iv derivation");

    /* 3. Continuous CTR: 96 sequential bytes sealed from block 0. */
    uint8_t plain[96];
    for (size_t i = 0; i < sizeof(plain); i++) {
        plain[i] = (uint8_t)i;
    }
    ap_legacy_ctr_t ctr;
    ap_legacy_ctr_start(&ctr, pv_key, pv_iv);
    uint8_t sealed[96];
    memcpy(sealed, plain, sizeof(sealed));
    ap_legacy_ctr_xor(&ctr, sealed, sizeof(sealed));
    expect_hex(sealed, CTR96_HEX, "ctr from block 0");

    /* 4. Sealing split across records must equal the unsplit run: the
     * keystream never resets at record boundaries. */
    ap_legacy_ctr_start(&ctr, pv_key, pv_iv);
    uint8_t split[96];
    memcpy(split, plain, sizeof(split));
    ap_legacy_ctr_xor(&ctr, split, 33);
    ap_legacy_ctr_xor(&ctr, split + 33, 63);
    expect(memcmp(split, sealed, sizeof(split)) == 0, "ctr continuity across records");

    /* 5. The pair-verify V2 variant: one keystream block skipped first. */
    ap_legacy_ctr_start(&ctr, pv_key, pv_iv);
    uint8_t skip[16];
    ap_legacy_ctr_xor(&ctr, skip, 16);
    memcpy(sealed, plain, sizeof(sealed));
    ap_legacy_ctr_xor(&ctr, sealed, sizeof(sealed));
    expect_hex(sealed, CTR96_SKIP_HEX, "ctr after skipping one block");

    /* 6. The legacy frame headers. */
    uint8_t header[AP_FRAME_HEADER_LEN];
    ap_frame_build_legacy(header, 0x010203, 0x01, 0, 1280, 720);
    expect(header[0] == 0x03 && header[1] == 0x02 && header[2] == 0x01 && header[3] == 0x00,
           "legacy codec record payload length");
    expect(header[4] == 0x01, "legacy codec record type");
    float width = 0;
    memcpy(&width, header + 16, 4);
    expect(width == 1280.0f, "legacy codec source width");
    memcpy(&width, header + 56, 4);
    expect(width == 1280.0f, "legacy codec display width");
    expect(header[8] == 0 && header[15] == 0, "legacy codec timestamp is zero");

    ap_frame_build_legacy(header, 0x0102, 0x00, 0x0000000500000000ULL, 0, 0);
    expect(header[4] == 0x00, "legacy video record type");
    expect(header[8] == 0 && header[9] == 0 && header[10] == 0 && header[11] == 0 &&
               header[12] == 0x05,
               "legacy video timestamp little-endian");

    /* 7. The SETUP plists round-trip through the reader. */
    uint8_t eiv[16];
    uint8_t ekey[72];
    for (size_t i = 0; i < sizeof(eiv); i++) {
        eiv[i] = (uint8_t)(0xA0 + i);
    }
    for (size_t i = 0; i < sizeof(ekey); i++) {
        ekey[i] = (uint8_t)(0xC0 + i);
    }
    uint8_t body[256];
    size_t body_len = ap_legacy_build_setup1(body, sizeof(body), eiv, ekey, 54321);
    expect(body_len > 0, "setup1 encodes");
    ap_plist_reader_t reader;
    expect(ap_plist_decode(&reader, body, body_len), "setup1 decodes");
    uint64_t port = 0;
    uint64_t field = 0;
    expect(ap_plist_dict_uint(&reader, ap_plist_root(&reader), "timingPort", &port) && port == 54321,
           "setup1 timingPort");
    /* eiv/ekey come back as data objects; walk them through the dict. */
    expect(ap_plist_dict_get(&reader, ap_plist_root(&reader), "eiv", &field), "setup1 has eiv");
    expect(ap_plist_dict_get(&reader, ap_plist_root(&reader), "ekey", &field), "setup1 has ekey");

    body_len = ap_legacy_build_setup2(body, sizeof(body), 0x0123456789ULL);
    expect(body_len > 0, "setup2 encodes");
    expect(ap_plist_decode(&reader, body, body_len), "setup2 decodes");
    uint64_t streams = 0;
    uint64_t stream = 0;
    expect(ap_plist_dict_get(&reader, ap_plist_root(&reader), "streams", &streams) &&
               ap_plist_array_count(&reader, streams) == 1 &&
               ap_plist_array_get(&reader, streams, 0, &stream),
           "setup2 streams array");
    expect(ap_plist_dict_uint(&reader, stream, "streamConnectionID", &port) &&
               port == 0x0123456789ULL,
           "setup2 streamConnectionID");
    expect(ap_plist_dict_uint(&reader, stream, "type", &port) && port == 110, "setup2 type 110");

    if (failures == 0) {
        printf("all legacy checks passed\n");
        return 0;
    }
    printf("%d legacy check(s) failed\n", failures);
    return 1;
}
