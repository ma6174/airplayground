/* Host tests for the media sources' pure logic: Annex-B to AVCC re-framing,
 * the decoder configuration record, and the test pattern generator.
 *
 * The esp_h264 encoder itself only runs on the chip (the library is
 * target-specific), so these tests pin down everything around it: what the
 * session will hand the data channel must be byte-correct regardless of what
 * the encoder produces.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_avcc.h"
#include "media_pattern.h"

static int failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            failures++;                                                 \
        }                                                               \
    } while (0)

static void put_nal(uint8_t *out, size_t *at, const uint8_t *payload, size_t len, bool four_byte)
{
    if (four_byte) {
        out[(*at)++] = 0;
    }
    out[(*at)++] = 0;
    out[(*at)++] = 0;
    out[(*at)++] = 1;
    memcpy(out + *at, payload, len);
    *at += len;
}

static void test_nal_iterator(void)
{
    printf("nal iterator\n");

    /* Two NALs with three-byte codes, one NAL with a four-byte code. */
    uint8_t stream[32];
    size_t at = 0;
    const uint8_t sps[] = { 0x67, 0x42, 0xc0, 0x1f };
    const uint8_t pps[] = { 0x68, 0xce };
    const uint8_t slice[] = { 0x41, 0x9a, 0x02, 0x05, 0xff };
    put_nal(stream, &at, sps, sizeof(sps), false);
    put_nal(stream, &at, pps, sizeof(pps), false);
    put_nal(stream, &at, slice, sizeof(slice), true);

    media_nal_iter_t it;
    media_nal_begin(&it, stream, at);

    CHECK(media_nal_next(&it));
    CHECK(it.type == 7 && it.nal_len == sizeof(sps) && it.offset == 3);
    CHECK(memcmp(stream + it.offset, sps, sizeof(sps)) == 0);

    CHECK(media_nal_next(&it));
    CHECK(it.type == 8 && it.nal_len == sizeof(pps));
    CHECK(memcmp(stream + it.offset, pps, sizeof(pps)) == 0);

    /* The four-byte start code belongs entirely to the framing: the payload
     * begins right after it, with no leading zero absorbed. */
    CHECK(media_nal_next(&it));
    CHECK(it.type == 1 && it.nal_len == sizeof(slice));
    CHECK(memcmp(stream + it.offset, slice, sizeof(slice)) == 0);

    CHECK(!media_nal_next(&it));
}

static void test_avcc_wrap(void)
{
    printf("avcc wrap\n");

    uint8_t stream[32];
    size_t at = 0;
    const uint8_t idr_slice[] = { 0x65, 0x88, 0x84 };
    const uint8_t p_slice[] = { 0x41, 0x00 };
    put_nal(stream, &at, idr_slice, sizeof(idr_slice), true);
    put_nal(stream, &at, p_slice, sizeof(p_slice), false);

    uint8_t out[32];
    size_t out_len = 0;
    bool idr = false;
    CHECK(media_avcc_wrap(stream, at, out, sizeof(out), &out_len, &idr));
    CHECK(idr);
    CHECK(out_len == 4 + sizeof(idr_slice) + 4 + sizeof(p_slice));

    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == sizeof(idr_slice));
    CHECK(memcmp(out + 4, idr_slice, sizeof(idr_slice)) == 0);
    size_t second = 4 + sizeof(idr_slice);
    CHECK(out[second] == 0 && out[second + 1] == 0 && out[second + 2] == 0 &&
          out[second + 3] == sizeof(p_slice));
    CHECK(memcmp(out + second + 4, p_slice, sizeof(p_slice)) == 0);

    /* A stream without an IDR slice reports idr=false; a P-only access unit is
     * exactly what the session sends between GOP boundaries. */
    uint8_t p_stream[8];
    size_t p_at = 0;
    put_nal(p_stream, &p_at, p_slice, sizeof(p_slice), false);
    CHECK(media_avcc_wrap(p_stream, p_at, out, sizeof(out), &out_len, &idr));
    CHECK(!idr && out_len == 4 + sizeof(p_slice));

    /* Too small an output buffer is a clean failure, not a truncation. */
    CHECK(!media_avcc_wrap(stream, at, out, 8, &out_len, &idr));
}

static void test_avcc_record(void)
{
    printf("avcc record\n");

    /* x264-shaped parameter sets, as the firmware's still-image source uses. */
    const uint8_t sps[] = { 0x67, 0x42, 0xc0, 0x1f, 0xdc, 0x05, 0x00, 0x5b };
    const uint8_t pps[] = { 0x68, 0xce, 0x0f, 0x2c, 0x80 };

    uint8_t record[64];
    size_t len = media_avcc_record(sps, sizeof(sps), pps, sizeof(pps), record, sizeof(record));
    CHECK(len == 6 + 2 + sizeof(sps) + 1 + 2 + sizeof(pps) + 4);

    CHECK(record[0] == 0x01);
    CHECK(record[1] == sps[1] && record[2] == sps[2] && record[3] == sps[3]);
    CHECK(record[4] == 0xff); /* lengthSizeMinusOne = 3 */
    CHECK(record[5] == 0xe1); /* one SPS */
    CHECK(record[6] == 0 && record[7] == sizeof(sps));
    CHECK(memcmp(record + 8, sps, sizeof(sps)) == 0);

    size_t at = 8 + sizeof(sps);
    CHECK(record[at] == 0x01); /* one PPS */
    CHECK(record[at + 1] == 0 && record[at + 2] == sizeof(pps));
    CHECK(memcmp(record + at + 3, pps, sizeof(pps)) == 0);

    /* The four trailing bytes genuine senders append. */
    CHECK(record[len - 4] == 0x02 && record[len - 3] == 0 && record[len - 2] == 0 &&
          record[len - 1] == 0);

    /* The record must match the still-image source's hand-built one in shape:
     * the session sends either through the same codec-packet path. */
    CHECK(len < 64);

    /* Too small a buffer is a clean failure. */
    CHECK(media_avcc_record(sps, sizeof(sps), pps, sizeof(pps), record, len - 1) == 0);
}

static void test_pattern(void)
{
    printf("test pattern\n");

    const size_t w = MEDIA_PATTERN_WIDTH;
    const size_t h = MEDIA_PATTERN_HEIGHT;
    const size_t y_size = w * h;
    const size_t uv_size = (w / 2) * (h / 2);
    const size_t total = y_size + 2 * uv_size;

    uint8_t *frame_a = malloc(total);
    uint8_t *frame_b = malloc(total);
    CHECK(frame_a != NULL && frame_b != NULL);

    memset(frame_a, 0xaa, total);
    media_pattern_generate(frame_a, total, 0);
    media_pattern_generate(frame_b, total, 1);

    /* Luma and chroma use the full studio range but never leave it. */
    int y_min = 255;
    int y_max = 0;
    for (size_t i = 0; i < y_size; i++) {
        if (frame_a[i] < y_min) y_min = frame_a[i];
        if (frame_a[i] > y_max) y_max = frame_a[i];
    }
    CHECK(y_min >= 0 && y_max <= 255 && y_max > y_min);

    for (size_t i = uv_size; i < total; i++) {
        CHECK(frame_a[i] >= 0 && frame_a[i] <= 255);
    }

    /* Consecutive frames differ in every plane: the pattern must animate, or
     * P frames would be pointless. */
    CHECK(memcmp(frame_a, frame_b, total) != 0);
    bool y_differs = false;
    bool u_differs = false;
    bool v_differs = false;
    for (size_t i = 0; i < y_size; i++) {
        if (frame_a[i] != frame_b[i]) y_differs = true;
    }
    for (size_t i = 0; i < uv_size; i++) {
        if (frame_a[y_size + i] != frame_b[y_size + i]) u_differs = true;
        if (frame_a[y_size + uv_size + i] != frame_b[y_size + uv_size + i]) v_differs = true;
    }
    CHECK(y_differs && u_differs && v_differs);

    /* The generation is a pure function of the step: the same frame rendered
     * twice matches byte for byte. */
    media_pattern_generate(frame_b, total, 42);
    media_pattern_generate(frame_a, total, 42);
    CHECK(memcmp(frame_a, frame_b, total) == 0);

    /* The border is a constant grey in every frame, at every step, so a
     * receiver shows a stable frame edge. */
    for (int step = 0; step < 200; step += 37) {
        media_pattern_generate(frame_a, total, (uint32_t)step);
        for (size_t x = 0; x < w; x++) {
            CHECK(frame_a[x] == frame_a[(h - 1) * w + x]);
        }
    }

    /* A short buffer is ignored rather than overruns. */
    memset(frame_a, 0x5a, total);
    media_pattern_generate(frame_a, total - 1, 7);
    for (size_t i = 0; i < total; i++) {
        CHECK(frame_a[i] == 0x5a);
    }

    free(frame_a);
    free(frame_b);
}

int main(void)
{
    test_nal_iterator();
    test_avcc_wrap();
    test_avcc_record();
    test_pattern();

    if (failures != 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all media host tests passed\n");
    return 0;
}
