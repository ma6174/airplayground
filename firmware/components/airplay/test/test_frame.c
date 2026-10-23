/* Host tests for the 128-byte video data-channel header.
 *
 * The receiver parses these bytes positionally, so every field is asserted at
 * its spec offset rather than through the builder's own helpers.
 */

#include <stdio.h>
#include <string.h>

#include "ap_frame.h"

static int failures;

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) {                                               \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                              \
        }                                                            \
    } while (0)

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *p)
{
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) {
        value = (value << 8) | p[i];
    }
    return value;
}

static float read_float_le(const uint8_t *p)
{
    uint32_t bits = read_u32_le(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void check_zero_tail(const uint8_t *header, int from, int to)
{
    for (int i = from; i < to; i++) {
        if (header[i] != 0) {
            printf("  FAIL header[%d] = 0x%02x, expected 0\n", i, header[i]);
            failures++;
        }
    }
}

static void test_codec_header(void)
{
    printf("test_codec_header\n");
    uint8_t header[AP_FRAME_HEADER_LEN];
    ap_frame_build_codec(header, 0x1234, 0x1122334455667788ULL, 1920, 1080);

    CHECK(read_u32_le(header + 0) == 0x1234);
    CHECK(header[4] == 0x01);
    CHECK(header[5] == 0x00);
    CHECK(header[6] == 0x16);
    CHECK(header[7] == 0x01);
    CHECK(read_u64_le(header + 8) == 0x1122334455667788ULL);
    CHECK(read_float_le(header + 16) == 1920.0f);
    CHECK(read_float_le(header + 20) == 1080.0f);
    check_zero_tail(header, 24, 40);
    CHECK(read_float_le(header + 40) == 1920.0f);
    CHECK(read_float_le(header + 44) == 1080.0f);
    CHECK(read_u64_le(header + 48) == 0);
    CHECK(read_float_le(header + 56) == 1920.0f);
    CHECK(read_float_le(header + 60) == 1080.0f);
    check_zero_tail(header, 64, 128);
}

static void test_video_header(void)
{
    printf("test_video_header\n");
    uint8_t header[AP_FRAME_HEADER_LEN];
    ap_frame_build_video(header, 4096 + 16, true, 0x0102030405060708ULL, 0);

    CHECK(read_u32_le(header + 0) == 4096 + 16);
    CHECK(header[4] == 0x00);
    CHECK(header[5] == 0x10);
    CHECK(header[6] == 0x00);
    CHECK(header[7] == 0x00);
    CHECK(read_u64_le(header + 8) == 0x0102030405060708ULL);
    check_zero_tail(header, 16, 40);
    CHECK(read_u64_le(header + 40) == 0);
    check_zero_tail(header, 48, 128);

    ap_frame_build_video(header, 100, false, 7, 0);
    CHECK(header[5] == 0x00);
}

static void test_heartbeat_header(void)
{
    printf("test_heartbeat_header\n");
    uint8_t header[AP_FRAME_HEADER_LEN];
    ap_frame_build_heartbeat(header);
    CHECK(read_u32_le(header + 0) == 0);
    CHECK(header[4] == 0x02);
    CHECK(header[5] == 0x00);
    CHECK(header[6] == 0x1e);
    CHECK(header[7] == 0x00);
    check_zero_tail(header, 8, 128);
}

static void test_idr_detection(void)
{
    printf("test_idr_detection\n");

    /* 4-byte big-endian NAL length, then the NAL header byte. */
    const uint8_t idr_frame[] = { 0x00, 0x00, 0x00, 0x03, 0x65, 0x88, 0x84 };
    CHECK(ap_avcc_has_idr(idr_frame, sizeof(idr_frame)));

    const uint8_t non_idr_frame[] = { 0x00, 0x00, 0x00, 0x03, 0x41, 0x9a, 0x22 };
    CHECK(!ap_avcc_has_idr(non_idr_frame, sizeof(non_idr_frame)));

    /* Truncated length prefix must not read past the buffer. */
    const uint8_t truncated[] = { 0x00, 0x00, 0x00, 0x10, 0x65 };
    CHECK(!ap_avcc_has_idr(truncated, sizeof(truncated)));

    const uint8_t empty[] = { 0 };
    CHECK(!ap_avcc_has_idr(empty, sizeof(empty) - 1));
}

int main(void)
{
    test_codec_header();
    test_video_header();
    test_heartbeat_header();
    test_idr_detection();

    if (failures != 0) {
        printf("test_frame: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_frame: ok\n");
    return 0;
}
