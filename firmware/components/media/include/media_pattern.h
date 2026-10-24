/*
 * The animated test pattern for the live video source.
 *
 * Everything is rendered directly in I420, the format the software encoder
 * consumes, with plain integer math: vertical colour bars drifting sideways,
 * a brighter horizontal band that travels down the picture, and a white block
 * bouncing across it. Consecutive frames differ everywhere, so P frames
 * genuinely carry residual data and a receiver shows real motion.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MEDIA_PATTERN_WIDTH 320
#define MEDIA_PATTERN_HEIGHT 180
#define MEDIA_PATTERN_FPS 10

/* Bytes of one I420 frame at the pattern resolution. */
#define MEDIA_PATTERN_YUV_LEN \
    ((size_t)MEDIA_PATTERN_WIDTH * MEDIA_PATTERN_HEIGHT * 3 / 2)

/* Renders frame number `step` into yuv (I420 planar). */
void media_pattern_generate(uint8_t *yuv, size_t len, uint32_t step);
