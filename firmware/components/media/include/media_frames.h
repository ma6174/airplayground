/*
 * Still images baked into flash as pre-encoded H.264 access units.
 *
 * tools/make_frames.py regenerates src/frames.c from a directory of images, so
 * the firmware never needs an encoder at runtime.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "airplay.h"

/* Fills a frame source over the baked-in images. */
const airplay_frames_t *media_still_frames(void);
