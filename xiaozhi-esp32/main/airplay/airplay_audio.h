#ifndef AIRPLAY_AUDIO_H
#define AIRPLAY_AUDIO_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "alac_frame.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_types.h"

#include <esp_err.h>

/**
 * Feeds the assistant's audio into the legacy AirPlay audio channel.
 *
 * The protocol stack pulls one ALAC frame per RTP packet on a 44.1 kHz beat and
 * sends nothing while the ring is empty (the receiver plays silence), so pauses
 * between sentences cost nothing. This sink resamples the codec-rate mono PCM
 * to 44.1 kHz, packs it into ALAC frames and buffers about 128 ms of them.
 * Push blocks while the ring is full, which paces the whole decode pipeline at
 * the receiver's clock instead of draining memory; when the sink is not live
 * the caller keeps the local speaker.
 */
class AirplayAudioSink {
public:
    static AirplayAudioSink& Instance();

    static constexpr int kRate = 44100;
    static constexpr int kSpf = kAlacSpf;

    /* Arms the sink for a session whose audio source runs at input_rate. */
    bool StartForSession(int input_rate);
    /* Disarms immediately and wakes pushers blocked on a full ring. */
    void StopForSession();

    /* Hands one playback chunk over; returns false when the local speaker must
     * play it instead: no live session, the beat stopped draining, or the
     * resampler failed. May block while the ring drains. */
    bool Push(const int16_t* mono, size_t samples);

    /* The protocol stack's pull: false when nothing is queued. The returned
     * pointer is consumed before the call returns, so it may point into the
     * ring. */
    bool PullFrame(const uint8_t** data, size_t* len);

    bool live() const { return live_.load(); }
    uint64_t pushed_frames() const { return pushed_frames_.load(); }
    uint64_t stalled_chunks() const { return stalled_chunks_.load(); }

private:
    AirplayAudioSink() = default;

    /* ~128 ms of audio: enough to hide scheduling jitter, small enough to keep
     * the delay between the device and the TV speaker short. */
    static constexpr size_t kSlots = 16;

    std::atomic<bool> live_{false};
    std::atomic<uint64_t> pushed_frames_{0};
    std::atomic<uint64_t> stalled_chunks_{0};

    std::mutex mutex_;
    std::condition_variable room_cv_;

    esp_ae_rate_cvt_handle_t resampler_ = nullptr;
    std::vector<int16_t> resample_buf_;
    /* 44.1 kHz samples waiting to fill a whole frame. */
    std::vector<int16_t> carry_;
    uint8_t ring_[kSlots][kAlacFrameBytes];
    size_t head_ = 0;   // next slot PullFrame hands out
    size_t count_ = 0;  // slots waiting in the ring
    uint64_t pulls_ = 0;
};

#endif  // AIRPLAY_AUDIO_H
