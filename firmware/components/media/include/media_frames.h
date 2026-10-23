/*
 * Content sources for the mirroring session.
 *
 * media_still_frames() replays pre-encoded H.264 access units baked into
 * flash by tools/make_frames.py - the baseline that works on any board.
 * media_live_source() encodes a test pattern at runtime with the esp_h264
 * software encoder; it needs about a megabyte of free memory and falls back
 * to the still images when the chip cannot provide it.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "airplay.h"

/* Fills a frame source over the baked-in images. */
const airplay_frames_t *media_still_frames(void);
