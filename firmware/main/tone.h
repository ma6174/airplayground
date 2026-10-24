/*
 * Audio-channel test tone: one ALAC frame of a 506.10 Hz sine, repeated. The
 * rhythm tells the two stream-key candidates apart, which is the only way to
 * hear which one a receiver accepts.
 */
#pragma once

#include <stddef.h>

#include "airplay.h"
#include "tone_frames.h"

size_t legacy_tone_frame(void *ctx, ap_audio_frame_t *frame);
void legacy_tone_reset(void);
