#include "media_text.h"

#include <stdio.h>
#include <string.h>

#include "media_pattern.h"

#ifndef ESP_PLATFORM
/* Host tests load the font image explicitly before rendering. */
static const uint8_t *s_font_image;
static size_t s_font_image_len;
#else
/* font16.bin rides in the firmware image; the symbol names come from the
 * file listed in the component's EMBED_FILES. */
extern const uint8_t _binary_font16_bin_start[];
extern const uint8_t _binary_font16_bin_end[];
static const uint8_t *s_font_image;
static size_t s_font_image_len;

/* Claims the embedded image on first use: the start/end symbols span an
 * extern array, so their difference is not a compile-time constant. */
static void ensure_embedded_font(void)
{
    static bool mapped;
    if (!mapped) {
        mapped = true;
        s_font_image = _binary_font16_bin_start;
        s_font_image_len = (size_t)(_binary_font16_bin_end - _binary_font16_bin_start);
    }
}
#endif

/* Video-range luma for background and glyphs; the chroma planes stay neutral
 * so the encoder sees plain white-on-black. Glyph coverage is 4 bits per
 * pixel, so the ramp in between has sixteen steps - that is what makes the
 * outlines smooth instead of staircased. */
#define TEXT_BG_Y 16
#define TEXT_FG_Y 235
#define TEXT_CHROMA 128
#define TEXT_COVERAGE 16
/* Marquee speed in pixels per frame. */
#define TEXT_SCROLL_PX 2

static struct {
    bool ok;
    uint8_t cell_w;
    uint8_t cell_h;
    uint16_t cell;
    uint16_t run_first;
    uint16_t run_count;
    uint16_t table_count;
    const uint8_t *run_bm;
    const uint8_t *table_cp;
    const uint8_t *table_bm;
    uint8_t shade[TEXT_COVERAGE];
} s_font;

/* The set text lives in two buffers: a writer fills the inactive one and
 * flips s_active, so the render thread always reads a complete string
 * without a lock. */
static char s_text[2][MEDIA_TEXT_MAX_BYTES];
static volatile int s_active;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static bool parse_font(void)
{
    if (s_font.ok) {
        return true;
    }
    if (s_font_image == NULL) {
#ifdef ESP_PLATFORM
        ensure_embedded_font();
#else
        return false;
#endif
    }
    const uint8_t *data = s_font_image;
    if (s_font_image_len < 16 || memcmp(data, "APF2", 4) != 0) {
        return false;
    }
    uint8_t cell_w = data[4];
    uint8_t cell_h = data[5];
    /* Odd widths would break the packed 2-pixel-per-byte coverage. */
    if (cell_w == 0 || cell_h == 0 || cell_w > 64 || cell_h > 64 || (cell_w & 1) != 0) {
        return false;
    }
    uint16_t cell = (uint16_t)(cell_w / 2 * cell_h + 1); /* coverage + advance */
    uint16_t run_first = le16(data + 6);
    uint16_t run_count = le16(data + 8);
    uint16_t table_count = le16(data + 10);
    size_t need = 16 + (size_t)cell * (run_count + table_count) + 2u * table_count;
    if (need > s_font_image_len) {
        return false;
    }

    s_font.cell_w = cell_w;
    s_font.cell_h = cell_h;
    s_font.cell = cell;
    s_font.run_first = run_first;
    s_font.run_count = run_count;
    s_font.table_count = table_count;
    s_font.run_bm = data + 16;
    s_font.table_cp = s_font.run_bm + (size_t)cell * run_count;
    s_font.table_bm = s_font.table_cp + 2u * table_count;
    for (int a = 0; a < TEXT_COVERAGE; a++) {
        s_font.shade[a] = (uint8_t)(TEXT_BG_Y +
                                    ((TEXT_FG_Y - TEXT_BG_Y) * a + (TEXT_COVERAGE / 2)) /
                                        (TEXT_COVERAGE - 1));
    }
    s_font.ok = true;
    return true;
}

bool media_text_load_font(const uint8_t *data, size_t len)
{
    if (data == NULL || len < 16 || memcmp(data, "APF2", 4) != 0) {
        return false;
    }
    s_font.ok = false;
    s_font_image = data;
    s_font_image_len = len;
    return parse_font();
}

/* The cell of one code point, or NULL when the font has none. The table is
 * sorted, so unknown code points cost a binary search. Each record is the
 * advance byte followed by the packed coverage. */
static const uint8_t *glyph_for(uint32_t cp)
{
    if (cp >= s_font.run_first && cp < (uint32_t)s_font.run_first + s_font.run_count) {
        return s_font.run_bm + (size_t)(cp - s_font.run_first) * s_font.cell;
    }
    uint16_t lo = 0;
    uint16_t hi = s_font.table_count;
    while (lo < hi) {
        uint16_t mid = (uint16_t)((lo + hi) / 2);
        uint16_t got = le16(s_font.table_cp + 2u * mid);
        if (got == cp) {
            return s_font.table_bm + (size_t)mid * s_font.cell;
        }
        if (got < cp) {
            lo = (uint16_t)(mid + 1);
        } else {
            hi = mid;
        }
    }
    return NULL;
}

/* Decodes one UTF-8 sequence and always advances the cursor by at least one
 * byte, so a malformed tail cannot stall the render loop; bad sequences come
 * back as U+FFFD, which the font does not carry and the renderer skips. */
static uint32_t next_cp(const char **cursor)
{
    const uint8_t *s = (const uint8_t *)*cursor;
    uint32_t cp;
    int extra;

    if (s[0] < 0x80) {
        *cursor += 1;
        return s[0];
    }
    if ((s[0] & 0xE0) == 0xC0) {
        cp = s[0] & 0x1Fu;
        extra = 1;
    } else if ((s[0] & 0xF0) == 0xE0) {
        cp = s[0] & 0x0Fu;
        extra = 2;
    } else if ((s[0] & 0xF8) == 0xF0) {
        cp = s[0] & 0x07u;
        extra = 3;
    } else {
        *cursor += 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *cursor += 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    *cursor += extra + 1;
    return cp;
}

bool media_text_generate(uint8_t *yuv, size_t len, uint32_t step)
{
    if (yuv == NULL || len < MEDIA_PATTERN_YUV_LEN || !parse_font()) {
        return false;
    }
    /* One volatile read: whatever set() flips afterwards belongs to the next
     * frame. */
    const char *text = s_text[s_active];
    if (text[0] == '\0') {
        return false;
    }

    const int w = MEDIA_PATTERN_WIDTH;
    const int h = MEDIA_PATTERN_HEIGHT;
    uint8_t *y_plane = yuv;
    uint8_t *u_plane = yuv + (size_t)w * h;
    uint8_t *v_plane = u_plane + (size_t)(w / 2) * (h / 2);

    int text_px = 0;
    for (const char *p = text; *p != '\0';) {
        const uint8_t *glyph = glyph_for(next_cp(&p));
        if (glyph != NULL) {
            text_px += glyph[0]; /* the byte before the coverage is the advance */
        }
    }
    if (text_px == 0) {
        return false;
    }

    int x0;
    if (text_px <= w) {
        x0 = (w - text_px) / 2;
    } else {
        /* Enter from the right, cross the picture, and let one picture width
         * of gap pass before the line returns. */
        uint32_t span = (uint32_t)text_px + (uint32_t)w;
        x0 = w - (int)(((uint64_t)step * TEXT_SCROLL_PX) % span);
    }
    int y0 = (h - s_font.cell_h) / 2;

    memset(y_plane, TEXT_BG_Y, (size_t)w * h);
    memset(u_plane, TEXT_CHROMA, (size_t)(w / 2) * (h / 2));
    memset(v_plane, TEXT_CHROMA, (size_t)(w / 2) * (h / 2));

    int pen = x0;
    for (const char *p = text; *p != '\0';) {
        const uint8_t *glyph = glyph_for(next_cp(&p));
        if (glyph == NULL) {
            continue;
        }
        const uint8_t *coverage = glyph + 1;
        for (int row = 0; row < s_font.cell_h; row++) {
            int y = y0 + row;
            if (y < 0 || y >= h) {
                continue;
            }
            const uint8_t *bits = coverage + (size_t)row * (s_font.cell_w / 2);
            uint8_t *out = y_plane + (size_t)y * w;
            for (int col = 0; col < s_font.cell_w; col++) {
                int x = pen + col;
                if (x < 0 || x >= w) {
                    continue;
                }
                uint8_t alpha = (col & 1) != 0 ? bits[col >> 1] & 0x0F
                                               : (uint8_t)(bits[col >> 1] >> 4);
                if (alpha != 0) {
                    out[x] = s_font.shade[alpha];
                }
            }
        }
        pen += glyph[0];
    }
    return true;
}

bool media_text_set(const char *utf8)
{
    if (utf8 == NULL) {
        utf8 = "";
    }
    size_t len = strlen(utf8);
    if (len >= MEDIA_TEXT_MAX_BYTES) {
        return false;
    }
    int next = 1 - s_active;
    memcpy(s_text[next], utf8, len + 1);
    s_active = next;
    return true;
}

void media_text_get(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    snprintf(buf, len, "%s", s_text[s_active]);
}
