#include "media_pattern.h"

#include <string.h>

/* A channel value from a wrapping triangle wave: every hue in one cycle gets a
 * distinct RGB triple. */
static uint8_t tri(int v)
{
    v &= 0xff;
    return (uint8_t)(v < 0x80 ? 2 * v : 2 * (0xff - v));
}

/* Full-range BT.601 conversion. The inputs keep every result inside 0..255. */
static uint8_t rgb_y(int r, int g, int b)
{
    return (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
}

static uint8_t rgb_u(int r, int g, int b)
{
    return (uint8_t)(((-43 * r - 85 * g + 128 * b) >> 8) + 128);
}

static uint8_t rgb_v(int r, int g, int b)
{
    return (uint8_t)(((128 * r - 107 * g - 21 * b) >> 8) + 128);
}

/* The colour of one pixel of frame `step`. Bars are 32 px wide and drift left;
 * the bouncing block and the border sit on top. */
static void pixel_rgb(int x, int y, uint32_t step, int *r, int *g, int *b)
{
    int bar = (int)(((x + (step & 0x3fff) * 4) % MEDIA_PATTERN_WIDTH) >> 5);
    int hue = bar * 28 + (int)(step * 5);
    *r = tri(hue);
    *g = tri(hue + 85);
    *b = tri(hue + 170);

    /* A travelling band dims everything it crosses, in half-steps so the
     * change stays visible after chroma subsampling. */
    int band = 40 + (int)(step % (MEDIA_PATTERN_HEIGHT - 80));
    if (y >= band && y < band + 24) {
        *r >>= 1;
        *g >>= 1;
        *b >>= 1;
    }

    if (x < 3 || x >= MEDIA_PATTERN_WIDTH - 3 || y < 3 || y >= MEDIA_PATTERN_HEIGHT - 3) {
        *r = 100;
        *g = 100;
        *b = 100;
        return;
    }

    int block_x = (int)((step * 7) % (MEDIA_PATTERN_WIDTH - 32));
    int block_y = (int)((step * 3) % (MEDIA_PATTERN_HEIGHT - 32));
    if (x >= block_x && x < block_x + 24 && y >= block_y && y < block_y + 24) {
        *r = 240;
        *g = 240;
        *b = 240;
    }
}

void media_pattern_generate(uint8_t *yuv, size_t len, uint32_t step)
{
    if (yuv == NULL || len < MEDIA_PATTERN_YUV_LEN) {
        return;
    }

    const int w = MEDIA_PATTERN_WIDTH;
    const int h = MEDIA_PATTERN_HEIGHT;
    uint8_t *y_plane = yuv;
    uint8_t *u_plane = yuv + (size_t)w * h;
    uint8_t *v_plane = u_plane + (size_t)(w / 2) * (h / 2);

    for (int y = 0; y < h; y++) {
        uint8_t *y_row = y_plane + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int r;
            int g;
            int b;
            pixel_rgb(x, y, step, &r, &g, &b);
            y_row[x] = rgb_y(r, g, b);
        }

        if ((y & 1) != 0) {
            continue;
        }
        uint8_t *u_row = u_plane + (size_t)(y / 2) * (w / 2);
        uint8_t *v_row = v_plane + (size_t)(y / 2) * (w / 2);
        for (int cx = 0; cx < w / 2; cx++) {
            int r;
            int g;
            int b;
            pixel_rgb(2 * cx, y, step, &r, &g, &b);
            u_row[cx] = rgb_u(r, g, b);
            v_row[cx] = rgb_v(r, g, b);
        }
    }
}
