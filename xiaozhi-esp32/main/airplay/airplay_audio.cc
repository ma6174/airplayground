#include "airplay_audio.h"

#include <chrono>

#include <esp_log.h>

namespace {
const char* const TAG = "AirplayAudio";
constexpr std::chrono::milliseconds kStallLimit{400};
constexpr std::chrono::milliseconds kRoomPoll{40};
}  // namespace

AirplayAudioSink& AirplayAudioSink::Instance() {
    static AirplayAudioSink instance;
    return instance;
}

bool AirplayAudioSink::StartForSession(int input_rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    count_ = 0;
    pulls_ = 0;
    carry_.clear();
    resample_buf_.clear();
    pushed_frames_ = 0;
    stalled_chunks_ = 0;
    if (resampler_ != nullptr) {
        esp_ae_rate_cvt_close(resampler_);
        resampler_ = nullptr;
    }
    if (input_rate > 0 && input_rate != kRate) {
        esp_ae_rate_cvt_cfg_t cfg = {
            .src_rate = (uint32_t)input_rate,
            .dest_rate = (uint32_t)kRate,
            .channel = (uint8_t)ESP_AUDIO_MONO,
            .bits_per_sample = ESP_AUDIO_BIT16,
            .complexity = 2,
            .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
        };
        if (esp_ae_rate_cvt_open(&cfg, &resampler_) != ESP_OK || resampler_ == nullptr) {
            ESP_LOGE(TAG, "cannot resample %d Hz to %d Hz", input_rate, kRate);
            return false;
        }
    }
    live_ = true;
    ESP_LOGI(TAG, "sink live: %d Hz mono -> %d Hz stereo, ring %d ms", input_rate, kRate,
             static_cast<int>(kSlots * kAlacSpf * 1000 / kRate));
    return true;
}

void AirplayAudioSink::StopForSession() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        live_ = false;
    }
    room_cv_.notify_all();
}

bool AirplayAudioSink::Push(const int16_t* mono, size_t samples) {
    if (!live_.load()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!live_.load()) {
            return false;
        }
        if (resampler_ != nullptr) {
            uint32_t max_out = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(resampler_, samples, &max_out);
            resample_buf_.resize(max_out);
            uint32_t actual = max_out;
            if (esp_ae_rate_cvt_process(resampler_, (esp_ae_sample_t)mono, samples,
                                        (esp_ae_sample_t)resample_buf_.data(),
                                        &actual) != ESP_OK) {
                ESP_LOGE(TAG, "resample failed");
                return false;
            }
            carry_.insert(carry_.end(), resample_buf_.begin(),
                          resample_buf_.begin() + actual);
        } else {
            carry_.insert(carry_.end(), mono, mono + samples);
        }
    }

    /* Emit whole frames, blocking on ring room: the beat that drains the ring
     * is the clock the receiver plays at, so this is what turns a decode that
     * could run at full speed into a realtime stream. If the beat disappears
     * (the session died without telling us), give the chunk back to the local
     * speaker instead of wedging the decode pipeline. */
    auto progress = std::chrono::steady_clock::now();
    for (;;) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!live_.load()) {
            carry_.clear();
            return false;
        }
        if (carry_.size() < kAlacSpf) {
            return true;
        }
        if (count_ == kSlots) {
            const uint64_t seen = pulls_;
            if (room_cv_.wait_for(lock, kRoomPoll) == std::cv_status::timeout) {
                if (pulls_ != seen) {
                    progress = std::chrono::steady_clock::now();
                } else if (std::chrono::steady_clock::now() - progress > kStallLimit) {
                    stalled_chunks_++;
                    carry_.clear();
                    ESP_LOGW(TAG, "audio beat stalled, back to the local speaker");
                    return false;
                }
                continue;
            }
        }
        AlacBuildFrame(carry_.data(), ring_[(head_ + count_) % kSlots]);
        carry_.erase(carry_.begin(), carry_.begin() + kAlacSpf);
        count_++;
        pushed_frames_++;
    }
}

bool AirplayAudioSink::PullFrame(const uint8_t** data, size_t* len) {
    std::lock_guard<std::mutex> lock(mutex_);
    pulls_++;
    if (count_ == 0) {
        return false;
    }
    *data = ring_[head_];
    *len = kAlacFrameBytes;
    head_ = (head_ + 1) % kSlots;
    count_--;
    room_cv_.notify_all();
    return true;
}
