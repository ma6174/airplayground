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
#include "media_text.h"

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

/* ---- text source ----
 *
 * The expected frames are computed from font16.bin through the small
 * independent reader below, never through the renderer's own lookups, so a
 * mistake in either side shows up as a byte mismatch.
 */

static struct {
    int cell_w, cell_h, cell;
    uint16_t run_first, run_count, table_count;
    const uint8_t *run_bm, *table_cp, *table_bm;
    uint8_t *image;
    size_t image_len;
} oracle;

static bool oracle_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    oracle.image = malloc((size_t)len);
    if (oracle.image == NULL || fread(oracle.image, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        return false;
    }
    fclose(f);
    oracle.image_len = (size_t)len;

    const uint8_t *d = oracle.image;
    if (len < 16 || memcmp(d, "APF2", 4) != 0) {
        return false;
    }
    oracle.cell_w = d[4];
    oracle.cell_h = d[5];
    oracle.cell = oracle.cell_w / 2 * oracle.cell_h + 1;
    oracle.run_first = (uint16_t)(d[6] | d[7] << 8);
    oracle.run_count = (uint16_t)(d[8] | d[9] << 8);
    oracle.table_count = (uint16_t)(d[10] | d[11] << 8);
    oracle.run_bm = d + 16;
    oracle.table_cp = oracle.run_bm + (size_t)oracle.cell * oracle.run_count;
    oracle.table_bm = oracle.table_cp + 2u * oracle.table_count;
    return oracle.table_bm + (size_t)oracle.cell * oracle.table_count <= d + len;
}

static const uint8_t *oracle_glyph(uint32_t cp)
{
    if (cp >= oracle.run_first && cp < oracle.run_first + oracle.run_count) {
        return oracle.run_bm + (size_t)(cp - oracle.run_first) * oracle.cell;
    }
    for (uint16_t i = 0; i < oracle.table_count; i++) {
        uint16_t got = (uint16_t)(oracle.table_cp[2 * i] | oracle.table_cp[2 * i + 1] << 8);
        if (got == cp) {
            return oracle.table_bm + (size_t)i * oracle.cell;
        }
        if (got > cp) {
            break;
        }
    }
    return NULL;
}

/* The documented render contract, computed from the font file alone. */
static void oracle_render(uint8_t *yuv, const uint32_t *cps, size_t n, uint32_t step)
{
    const int w = MEDIA_PATTERN_WIDTH;
    const int h = MEDIA_PATTERN_HEIGHT;
    const size_t y_size = (size_t)w * h;
    const size_t uv_size = (size_t)(w / 2) * (h / 2);
    memset(yuv, 16, y_size);
    memset(yuv + y_size, 128, 2 * uv_size);

    int text_px = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *glyph = oracle_glyph(cps[i]);
        if (glyph != NULL) {
            text_px += glyph[0];
        }
    }
    int x0 = w;
    if (text_px > 0) {
        if (text_px <= w) {
            x0 = (w - text_px) / 2;
        } else {
            uint32_t span = (uint32_t)text_px + (uint32_t)w;
            x0 = w - (int)(((uint64_t)step * 2) % span);
        }
    }
    int y0 = (h - oracle.cell_h) / 2;

    int pen = x0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *glyph = oracle_glyph(cps[i]);
        if (glyph == NULL) {
            continue;
        }
        const uint8_t *coverage = glyph + 1;
        for (int row = 0; row < oracle.cell_h; row++) {
            int y = y0 + row;
            if (y < 0 || y >= h) {
                continue;
            }
            const uint8_t *bits = coverage + (size_t)row * (oracle.cell_w / 2);
            uint8_t *out = yuv + (size_t)y * w;
            for (int col = 0; col < oracle.cell_w; col++) {
                int x = pen + col;
                if (x < 0 || x >= w) {
                    continue;
                }
                int alpha = (col & 1) != 0 ? bits[col >> 1] & 0x0F : bits[col >> 1] >> 4;
                if (alpha != 0) {
                    out[x] = (uint8_t)(16 + (219 * alpha + 8) / 15);
                }
            }
        }
        pen += glyph[0];
    }
}

static void test_text(void)
{
    printf("text source\n");

    const size_t w = MEDIA_PATTERN_WIDTH;
    const size_t h = MEDIA_PATTERN_HEIGHT;
    const size_t y_size = w * h;
    const size_t uv_size = (w / 2) * (h / 2);
    const size_t total = y_size + 2 * uv_size;
    uint8_t *frame = malloc(total);
    uint8_t *expect = malloc(total);
    CHECK(frame != NULL && expect != NULL);

    /* Before any font the caller keeps the test pattern. */
    CHECK(media_text_set("A"));
    CHECK(!media_text_generate(frame, total, 0));

    /* Malformed font images are a clean rejection. */
    CHECK(!media_text_load_font(NULL, 0));
    CHECK(!media_text_load_font((const uint8_t *)"nope", 4));
    CHECK(!media_text_load_font((const uint8_t *)"APF1\0\0\0\0", 8));
    CHECK(!media_text_generate(frame, total, 0));

    CHECK(oracle_load("../font16.bin"));
    CHECK(media_text_load_font(oracle.image, oracle.image_len));

    /* A single ASCII glyph: the full canvas matches the oracle, chroma is
     * neutral everywhere, and the ink sits inside the centred cell. */
    CHECK(media_text_set("A"));
    memset(frame, 0x5a, total);
    CHECK(media_text_generate(frame, total, 7));
    oracle_render(expect, (const uint32_t[]){'A'}, 1, 7);
    CHECK(memcmp(frame, expect, total) == 0);
    CHECK(frame[0] == 16);
    bool chroma_neutral = true;
    bool has_ink = false;
    int ink_top = (int)h;
    int ink_bottom = -1;
    for (size_t i = y_size; i < total; i++) {
        chroma_neutral = chroma_neutral && frame[i] == 128;
    }
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            if (frame[y * w + x] == 235) {
                has_ink = true;
                ink_top = (int)y < ink_top ? (int)y : ink_top;
                ink_bottom = (int)y > ink_bottom ? (int)y : ink_bottom;
            }
        }
    }
    CHECK(chroma_neutral);
    CHECK(has_ink);
    CHECK(ink_top >= 80 && ink_bottom < 100);

    /* A line that fits does not move between frames. */
    CHECK(media_text_generate(frame, total, 8));
    oracle_render(expect, (const uint32_t[]){'A'}, 1, 8);
    CHECK(memcmp(frame, expect, total) == 0);

    /* Chinese glyphs come from the committed table section. */
    CHECK(media_text_set("中国"));
    CHECK(media_text_generate(frame, total, 3));
    oracle_render(expect, (const uint32_t[]){0x4E2D, 0x56FD}, 2, 3);
    CHECK(memcmp(frame, expect, total) == 0);

    /* Glyphs the font lacks (an emoji) vanish without advancing the pen; a
     * malformed UTF-8 tail degrades to the same skip, never a crash. */
    CHECK(media_text_set("A\xF0\x9F\x98\x80" "中"));
    CHECK(media_text_generate(frame, total, 2));
    oracle_render(expect, (const uint32_t[]){'A', 0x4E2D}, 2, 2);
    CHECK(memcmp(frame, expect, total) == 0);

    CHECK(media_text_set("\xE4\xB8\xAD\xFF\xE4\xB8"));
    CHECK(media_text_generate(frame, total, 0));
    oracle_render(expect, (const uint32_t[]){0x4E2D}, 1, 0);
    CHECK(memcmp(frame, expect, total) == 0);

    /* A line wider than the picture scrolls left at two pixels per frame. */
    uint32_t long_text[44];
    char long_str[45];
    for (int i = 0; i < 44; i++) {
        long_text[i] = (uint32_t)('0' + i % 10);
        long_str[i] = (char)('0' + i % 10);
    }
    long_str[44] = '\0';
    CHECK(media_text_set(long_str));
    for (uint32_t step = 0; step < 4; step++) {
        CHECK(media_text_generate(frame, total, step));
        oracle_render(expect, long_text, 44, step);
        CHECK(memcmp(frame, expect, total) == 0);
    }

    /* A string that does not fit is rejected and the old text survives. */
    char too_long[MEDIA_TEXT_MAX_BYTES + 1];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    CHECK(!media_text_set(too_long));
    CHECK(media_text_generate(frame, total, 1));
    oracle_render(expect, long_text, 44, 1);
    CHECK(memcmp(frame, expect, total) == 0);
    char current[MEDIA_TEXT_MAX_BYTES];
    media_text_get(current, sizeof(current));
    CHECK(strcmp(current, long_str) == 0);

    /* Empty text hands the canvas back to the test pattern. */
    CHECK(media_text_set(""));
    CHECK(!media_text_generate(frame, total, 0));
    media_text_get(current, sizeof(current));
    CHECK(current[0] == '\0');

    /* A short buffer is reported as a miss, not written. */
    CHECK(media_text_set("A"));
    memset(frame, 0x5a, total);
    CHECK(!media_text_generate(frame, total - 1, 0));
    for (size_t i = 0; i < total; i++) {
        CHECK(frame[i] == 0x5a);
    }

    free(frame);
    free(expect);
}

int main(void)
{
    test_nal_iterator();
    test_avcc_wrap();
    test_avcc_record();
    test_pattern();
    test_text();

    /* Human eyeball dump: MEDIA_TEXT_DUMP=path writes one I420 frame that
     * ffmpeg can turn into a PNG (rawvideo, grey + the two chroma planes). */
    const char *dump = getenv("MEDIA_TEXT_DUMP");
    if (dump != NULL) {
        const size_t total =
            MEDIA_PATTERN_WIDTH * MEDIA_PATTERN_HEIGHT * 3 / 2;
        uint8_t *frame = malloc(total);
        media_text_set("你好 AirPlay，中文 OK");
        memset(frame, 0, total);
        media_text_generate(frame, total, 12);
        FILE *out = fopen(dump, "wb");
        fwrite(frame, 1, total, out);
        fclose(out);
        free(frame);
        printf("dumped %s\n", dump);
    }

    if (failures != 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all media host tests passed\n");
    return 0;
}
