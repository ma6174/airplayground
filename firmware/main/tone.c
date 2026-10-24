#include "tone.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_timer.h"

#include "tone_frames.h"

/* One window per key candidate: the bare-key candidate plays the low tone, the
 * mixed-key one the tone an octave up. Both run continuously, so the listener
 * hears which candidate the receiver can decode and how long each window lasts
 * without depending on how the receiver renders gaps. */
#define TONE_WINDOW_MS 4000u

static int64_t s_start_us;

void legacy_tone_reset(void)
{
    s_start_us = 0;
}

size_t legacy_tone_frame(void *ctx, ap_audio_frame_t *frame)
{
    (void)ctx;
    if (s_start_us == 0) {
        s_start_us = esp_timer_get_time();
    }
    uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - s_start_us) / 1000);
    bool mixed = (elapsed_ms / TONE_WINDOW_MS) % 2u == 1u;
    frame->data = mixed ? k_tone_high : k_tone_low;
    frame->len = mixed ? sizeof(k_tone_high) : sizeof(k_tone_low);
    frame->mixed_key = mixed;
    return frame->len;
}
