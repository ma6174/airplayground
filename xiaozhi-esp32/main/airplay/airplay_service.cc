#include "airplay_service.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <nvs.h>

#include "board.h"
#include "airplay_audio.h"
#include "airplay_http.h"
#include "audio_codec.h"
#include "cJSON.h"
#include "display/virtual_display.h"
#include "settings.h"
#include "webcast_client.h"

#include "esp_h264_enc_param.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_sw.h"
#include "esp_h264_types.h"

extern "C" {
#include "airplay.h"
#include "ap_discovery.h"
#include "media_avcc.h"
#include "media_net.h"
}

#define TAG "Airplay"

namespace {

/* A receiver is addressed either directly ("192.168.1.35", "192.168.1.35:7001")
 * or by the name the discovery list shows ("我的Play 5"). */
bool LooksLikeAddress(const std::string& value) {
    if (value.empty() || value[0] < '0' || value[0] > '9') {
        return false;
    }
    for (char c : value) {
        if ((c < '0' || c > '9') && c != '.' && c != ':') {
            return false;
        }
    }
    return true;
}

constexpr size_t kAuCap = 128 * 1024;
constexpr size_t kAvccCap = 64;
/* Every encoded frame is a keyframe. The virtual display only re-encodes when
 * the UI changes, so the rate cost is small, and a keyframe carries no reference
 * chain: a receiver that mishandles P-frames cannot smear the picture, and the
 * unchanged-screen resend keeps replaying a self-contained frame. */
constexpr uint8_t kGop = 1;
constexpr uint32_t kBitrateBps = 3000000;
constexpr uint8_t kQpMin = 26;
constexpr uint8_t kQpMax = 42;
/* 352 samples per packet: 8 ms of ALAC, the size the airplay component was
 * measured with on the projector (4096-sample packets overflow the Wi-Fi
 * buffers and end the session). */
constexpr uint16_t kAudioSpf = 352;

/* One screen-to-H.264 pipeline. It lives for the whole session: the protocol
 * stack asks for the decoder configuration record once and then for one access
 * unit per frame tick. */
struct Sink {
    VirtualDisplay* display = nullptr;
    esp_h264_enc_handle_t enc = nullptr;
    int width = 0;
    int height = 0;
    int fps = 15;
    uint8_t* yuv = nullptr;
    uint8_t* annexb = nullptr;
    uint8_t* au = nullptr;
    uint8_t avcc[kAvccCap] = {0};
    size_t avcc_len = 0;
    bool have_pending = false;
    bool au_valid = false;
    bool au_is_idr = false;
    size_t au_len = 0;
    uint32_t frame_index = 0;
    uint32_t pts_ms = 0;
    uint32_t seen_counter = 0;
};

Sink s_sink;
airplay_live_source_t s_source;

inline uint8_t ClampU8(int value) {
    return value < 0 ? 0 : (value > 255 ? 255 : static_cast<uint8_t>(value));
}

/* RGB565 -> planar YUV420 (BT.601 studio swing, integer only): the layout the
 * software encoder takes. */
void Rgb565ToI420(const uint16_t* src, int width, int height, uint8_t* y_plane, uint8_t* u_plane,
                  uint8_t* v_plane) {
    for (int row = 0; row < height; row++) {
        const uint16_t* pixels = src + static_cast<size_t>(row) * width;
        uint8_t* y_row = y_plane + static_cast<size_t>(row) * width;
        for (int col = 0; col < width; col++) {
            const uint16_t p = pixels[col];
            const int r = ((p >> 11) & 0x1F) * 255 / 31;
            const int g = ((p >> 5) & 0x3F) * 255 / 63;
            const int b = (p & 0x1F) * 255 / 31;
            y_row[col] = static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
        }
    }

    const int chroma_w = width / 2;
    const int chroma_h = height / 2;
    for (int cy = 0; cy < chroma_h; cy++) {
        for (int cx = 0; cx < chroma_w; cx++) {
            int r_sum = 0;
            int g_sum = 0;
            int b_sum = 0;
            for (int dy = 0; dy < 2; dy++) {
                const uint16_t* pixels =
                    src + static_cast<size_t>(cy * 2 + dy) * width + static_cast<size_t>(cx * 2);
                for (int dx = 0; dx < 2; dx++) {
                    const uint16_t p = pixels[dx];
                    r_sum += ((p >> 11) & 0x1F) * 255 / 31;
                    g_sum += ((p >> 5) & 0x3F) * 255 / 63;
                    b_sum += (p & 0x1F) * 255 / 31;
                }
            }
            const int r = r_sum / 4;
            const int g = g_sum / 4;
            const int b = b_sum / 4;
            u_plane[cy * chroma_w + cx] = ClampU8(((-43 * r - 85 * g + 128 * b) >> 8) + 128);
            v_plane[cy * chroma_w + cx] = ClampU8(((128 * r - 107 * g - 21 * b) >> 8) + 128);
        }
    }
}

bool OpenEncoder(esp_h264_enc_handle_t* out, int width, int height, int fps, uint8_t gop) {
    esp_h264_enc_cfg_t cfg = {};
    cfg.pic_type = ESP_H264_RAW_FMT_I420;
    cfg.gop = gop;
    cfg.fps = static_cast<uint8_t>(fps);
    cfg.res.width = static_cast<uint16_t>(width);
    cfg.res.height = static_cast<uint16_t>(height);
    cfg.rc.bitrate = kBitrateBps;
    cfg.rc.qp_min = kQpMin;
    cfg.rc.qp_max = kQpMax;

    esp_h264_err_t err = esp_h264_enc_sw_new(&cfg, out);
    if (err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "cannot create the encoder (%d)", err);
        *out = nullptr;
        return false;
    }
    err = esp_h264_enc_open(*out);
    if (err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "cannot open the encoder (%d)", err);
        esp_h264_enc_del(*out);
        *out = nullptr;
        return false;
    }
    return true;
}

void CloseEncoder() {
    if (s_sink.enc != nullptr) {
        esp_h264_enc_close(s_sink.enc);
        esp_h264_enc_del(s_sink.enc);
        s_sink.enc = nullptr;
    }
}

bool EncodeOne(size_t* annexb_len, bool* idr) {
    if (s_sink.display == nullptr || s_sink.enc == nullptr) {
        return false;
    }
    const int width = s_sink.width;
    const int height = s_sink.height;
    const size_t luma = static_cast<size_t>(width) * height;

    /* LVGL draws straight into the frame buffer; convert under the render lock
     * so the encoder sees one consistent frame instead of a mid-redraw tear.
     * The converted buffer is also what the hardware encoder DMAs from. */
    s_sink.display->LockRender();
    Rgb565ToI420(s_sink.display->framebuffer(), width, height, s_sink.yuv, s_sink.yuv + luma,
                 s_sink.yuv + luma + luma / 4);
    s_sink.display->UnlockRender();

    esp_h264_enc_in_frame_t in = {};
    in.raw_data.buffer = s_sink.yuv;
    in.raw_data.len = static_cast<uint32_t>(luma * 3 / 2);
    in.pts = s_sink.pts_ms;

    esp_h264_enc_out_frame_t out = {};
    out.raw_data.buffer = s_sink.annexb;
    out.raw_data.len = static_cast<uint32_t>(kAuCap);

    esp_h264_err_t err = esp_h264_enc_process(s_sink.enc, &in, &out);
    if (err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "encode failed (%d)", err);
        return false;
    }
    if (out.frame_type == ESP_H264_FRAME_TYPE_INVALID || out.length == 0) {
        ESP_LOGE(TAG, "encoder produced no frame");
        return false;
    }

    s_sink.frame_index++;
    s_sink.pts_ms += static_cast<uint32_t>(1000 / (s_sink.fps > 0 ? s_sink.fps : 15));
    *annexb_len = out.length;
    *idr = out.frame_type == ESP_H264_FRAME_TYPE_IDR;
    return true;
}

bool CaptureCodecRecord(size_t annexb_len) {
    const uint8_t* sps = nullptr;
    const uint8_t* pps = nullptr;
    size_t sps_len = 0;
    size_t pps_len = 0;

    media_nal_iter_t it;
    media_nal_begin(&it, s_sink.annexb, annexb_len);
    while (media_nal_next(&it)) {
        if (it.type == 7 && sps == nullptr) {
            sps = s_sink.annexb + it.offset;
            sps_len = it.nal_len;
        } else if (it.type == 8 && pps == nullptr) {
            pps = s_sink.annexb + it.offset;
            pps_len = it.nal_len;
        }
    }
    if (sps == nullptr || pps == nullptr) {
        ESP_LOGE(TAG, "first encode produced no SPS/PPS");
        return false;
    }
    s_sink.avcc_len = media_avcc_record(sps, sps_len, pps, pps_len, s_sink.avcc, kAvccCap);
    if (s_sink.avcc_len == 0) {
        ESP_LOGE(TAG, "codec record does not fit %u bytes", (unsigned)kAvccCap);
        return false;
    }
    return true;
}

/* Called once per session, before the first frame. Rebuilding the encoder here
 * guarantees the receiver's first frame is an IDR with fresh parameter sets. */
const uint8_t* SourceCodecRecord(void* user, size_t* len) {
    (void)user;
    if (s_sink.display == nullptr) {
        return nullptr;
    }
    s_sink.width = s_sink.display->width();
    s_sink.height = s_sink.display->height();
    s_sink.fps = s_sink.display->fps();
    if ((s_sink.width & 1) != 0 || (s_sink.height & 1) != 0) {
        ESP_LOGE(TAG, "frame size %dx%d must be even for YUV420", s_sink.width, s_sink.height);
        return nullptr;
    }

    const size_t luma = static_cast<size_t>(s_sink.width) * s_sink.height;
    if (s_sink.yuv == nullptr) {
        s_sink.yuv =
            static_cast<uint8_t*>(heap_caps_aligned_alloc(64, luma * 3 / 2, MALLOC_CAP_SPIRAM));
        s_sink.annexb = static_cast<uint8_t*>(heap_caps_malloc(kAuCap, MALLOC_CAP_SPIRAM));
        s_sink.au = static_cast<uint8_t*>(heap_caps_malloc(kAuCap, MALLOC_CAP_SPIRAM));
    }
    if (s_sink.yuv == nullptr || s_sink.annexb == nullptr || s_sink.au == nullptr) {
        ESP_LOGE(TAG, "no PSRAM for the encoder buffers (%u bytes luma)", (unsigned)luma);
        return nullptr;
    }

    CloseEncoder();
    s_sink.have_pending = false;
    s_sink.au_valid = false;
    s_sink.frame_index = 0;
    s_sink.pts_ms = 0;

    if (!OpenEncoder(&s_sink.enc, s_sink.width, s_sink.height, s_sink.fps, kGop)) {
        return nullptr;
    }

    bool idr = false;
    size_t annexb_len = 0;
    if (!EncodeOne(&annexb_len, &idr) || !idr) {
        ESP_LOGE(TAG, "the first encode is not an IDR; encoder state is broken");
        CloseEncoder();
        return nullptr;
    }
    if (!CaptureCodecRecord(annexb_len)) {
        CloseEncoder();
        return nullptr;
    }
    if (!media_avcc_wrap(s_sink.annexb, annexb_len, s_sink.au, kAuCap, &s_sink.au_len, &idr)) {
        CloseEncoder();
        return nullptr;
    }
    s_sink.au_is_idr = true;
    s_sink.au_valid = true;

    ESP_LOGI(TAG, "video source ready: %dx%d @ %d fps, avcC %u bytes", s_sink.width, s_sink.height,
             s_sink.fps, (unsigned)s_sink.avcc_len);
    s_sink.have_pending = true;
    s_sink.seen_counter = s_sink.display->frame_counter();
    *len = s_sink.avcc_len;
    return s_sink.avcc;
}

esp_err_t SourceNextFrame(void* user, const uint8_t** au, size_t* len, bool* idr) {
    (void)user;
    if (s_sink.display == nullptr || s_sink.enc == nullptr) {
        return ESP_FAIL;
    }

    if (s_sink.have_pending) {
        s_sink.have_pending = false;
        *au = s_sink.au;
        *len = s_sink.au_len;
        *idr = true;
        return ESP_OK;
    }

    const uint32_t counter = s_sink.display->frame_counter();
    if (counter == s_sink.seen_counter && s_sink.au_valid) {
        /* The screen did not change: resend the previous access unit instead of
         * re-encoding. Static text costs nothing. */
        *au = s_sink.au;
        *len = s_sink.au_len;
        *idr = s_sink.au_is_idr;
        return ESP_OK;
    }
    s_sink.seen_counter = counter;

    size_t annexb_len = 0;
    bool is_idr = false;
    if (!EncodeOne(&annexb_len, &is_idr)) {
        return ESP_FAIL;
    }
    if (!media_avcc_wrap(s_sink.annexb, annexb_len, s_sink.au, kAuCap, &s_sink.au_len, &is_idr)) {
        ESP_LOGE(TAG, "access unit does not fit %u bytes", (unsigned)kAuCap);
        return ESP_FAIL;
    }
    s_sink.au_valid = true;
    s_sink.au_is_idr = is_idr;
    *au = s_sink.au;
    *len = s_sink.au_len;
    *idr = is_idr;
    return ESP_OK;
}

/* HAP long-term identity, persisted so receivers recognise the device across
 * reboots (pairing then only needs pair-verify). */
esp_err_t LoadIdentity(hap_identity_t* identity) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("airplay", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t seed[HAP_ED25519_SEED_LEN];
    size_t seed_len = sizeof(seed);
    char uuid[HAP_ID_LEN + 1];
    size_t uuid_len = sizeof(uuid);

    err = nvs_get_blob(handle, "id_seed", seed, &seed_len);
    const esp_err_t uuid_err = nvs_get_str(handle, "id_uuid", uuid, &uuid_len);
    if (err == ESP_OK && uuid_err == ESP_OK && seed_len == sizeof(seed)) {
        nvs_close(handle);
        if (hap_identity_from_seed(identity, seed, uuid) == 0) {
            ESP_LOGI(TAG, "identity %s", identity->pairing_id);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "stored identity is unusable, creating a new one");
        err = nvs_open("airplay", NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            return err;
        }
    }

    hap_identity_generate(identity);
    err = nvs_set_blob(handle, "id_seed", identity->seed, sizeof(identity->seed));
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "id_uuid", identity->pairing_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "new identity %s", identity->pairing_id);
    } else {
        ESP_LOGE(TAG, "cannot persist the identity: %s", esp_err_to_name(err));
    }
    return err;
}

void GetLocalIp(char* out, size_t len) {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
        snprintf(out, len, IPSTR, IP2STR(&info.ip));
    } else {
        snprintf(out, len, "0.0.0.0");
    }
}

/* The protocol stack pulls one frame per packet; the packet builder consumes
 * the ring slot's bytes before the call returns, so handing out a pointer into
 * the ring is safe. Bare stream key: the projector derives nothing (the same
 * choice legacy_key_mode 0 makes for the mirror stream). */
size_t SinkAudioFrame(void* ctx, ap_audio_frame_t* frame) {
    const uint8_t* data = nullptr;
    size_t len = 0;
    if (!static_cast<AirplayAudioSink*>(ctx)->PullFrame(&data, &len)) {
        return 0;
    }
    frame->data = data;
    frame->len = len;
    frame->mixed_key = false;
    return len;
}

/* One audio frame the capture client sent, per packet the session sends. An
 * empty ring yields no frame: a made-up payload decodes as noise. The
 * projector keys the audio stream off the bare stream key. */
size_t CaptureAudioFrame(void* ctx, ap_audio_frame_t* frame) {
    (void)ctx;
    size_t len = 0;
    const uint8_t* data = media_net_audio(&len);
    if (data == nullptr || len == 0) {
        return 0;
    }
    frame->data = data;
    frame->len = len;
    frame->mixed_key = false;
    return len;
}

}  // namespace

AirplayService& AirplayService::GetInstance() {
    static AirplayService instance;
    return instance;
}

void AirplayService::AttachDisplay(VirtualDisplay* display) {
    display_ = display;
    s_sink.display = display;

    s_source.name = "virtual display";
    s_source.width = static_cast<uint16_t>(display->width());
    s_source.height = static_cast<uint16_t>(display->height());
    s_source.interval_ms = static_cast<uint32_t>(1000 / (display->fps() > 0 ? display->fps() : 15));
    s_source.max_au_len = kAuCap;
    s_source.codec_record = SourceCodecRecord;
    s_source.next_frame = SourceNextFrame;
    s_source.user = nullptr;
}

std::string AirplayService::ResolveHost() const {
    Settings settings("airplay", false);
    std::string host = settings.GetString("host", "");
    if (host.empty()) {
        host = CONFIG_AIRPLAY_DEFAULT_HOST;
    }
    return host;
}

bool AirplayService::ResolveLegacy() const {
    Settings settings("airplay", false);
    return settings.GetString("legacy", "0") == "1";
}

bool AirplayService::ResolveAudioEnabled() const {
    Settings settings("airplay", false);
    return settings.GetString("audio", "1") == "1";
}

/* Stores the effective target (with port) and transport, so "the receiver used
 * last time" resolves to the same address the same way. */
void AirplayService::Remember(const std::string& host, bool legacy) const {
    Settings settings("airplay", true);
    settings.SetString("host", host);
    settings.SetString("legacy", legacy ? "1" : "0");
}

bool AirplayService::Start(const std::string& host, bool legacy) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "a session is already running");
        return false;
    }

    std::string target = host;
    if (target.empty()) {
        target = ResolveHost();
        /* Asked to reuse the last receiver: it was remembered together with the
         * way it has to be spoken to (a projector only answers legacy AirTunes). */
        legacy = legacy || ResolveLegacy();
    }
    name_.clear();
    /* A named receiver resolves through the discovery list, which also decides
     * the transport: AppleTV3,2-style boxes only speak legacy AirTunes. */
    if (!target.empty() && !LooksLikeAddress(target)) {
        airplay_target_t found = {};
        if (!ap_discovery_find(target.c_str(), &found)) {
            ESP_LOGE(TAG, "no receiver named \"%s\"; self.airplay.discover lists them",
                     target.c_str());
            running_ = false;
            return false;
        }
        name_ = found.name;
        legacy = legacy || found.legacy;
        target = std::string(found.host) + ":" + std::to_string(found.port);
        ESP_LOGI(TAG, "\"%s\" resolved to %s (%s)", found.name, target.c_str(),
                 found.legacy ? "legacy AirTunes" : "HAP");
    }
    if (target.empty()) {
        ESP_LOGE(TAG, "no receiver address; pass one in or set CONFIG_AIRPLAY_DEFAULT_HOST");
        running_ = false;
        return false;
    }

    /* "host:port" for receivers whose RTSP does not live on 7001/7000; the
     * projector advertises 7001, the real RTSP service answers on 7002. */
    uint16_t port = 7000;
    const auto colon = target.rfind(':');
    if (colon != std::string::npos && target.find(':') == colon) {
        int parsed = atoi(target.c_str() + colon + 1);
        if (parsed > 0 && parsed < 65536) {
            port = static_cast<uint16_t>(parsed);
            target.resize(colon);
        }
    }

    host_ = target;
    port_ = port;
    legacy_ = legacy;
    audio_enabled_ = ResolveAudioEnabled();
    stop_requested_ = false;
    pin_timeout_ = false;
    pin_pending_ = false;
    last_result_ = "starting";

    BaseType_t created = xTaskCreate(SessionTask, "airplay", 16 * 1024, this, 3, &task_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "cannot create the session task");
        task_ = nullptr;
        running_ = false;
        return false;
    }
    ESP_LOGI(TAG, "connecting to %s:%u (%s)", host_.c_str(), (unsigned)port_,
             legacy_ ? "legacy AirTunes" : "HAP");
    return true;
}

void AirplayService::Stop() {
    if (!running_.load()) {
        return;
    }
    ESP_LOGI(TAG, "stop requested");
    stop_requested_ = true;
    airplay_request_stop();
}

bool AirplayService::AudioOutput(std::vector<int16_t>& pcm) {
    /* While a capture client is on screen it owns the TV's audio channel with
     * its own frames; the assistant's voice stays on the local speaker. */
    if (!running_.load() || !audio_enabled_.load() || !legacy_ || capture_source_.load()) {
        return false;
    }
    return AirplayAudioSink::Instance().Push(pcm.data(), pcm.size());
}

void AirplayService::SetAudioEnabled(bool enabled) {
    {
        Settings settings("airplay", true);
        settings.SetString("audio", enabled ? "1" : "0");
    }
    audio_enabled_ = enabled;
    ESP_LOGI(TAG, "audio to the TV: %s", enabled ? "on" : "off");
    /* The audio stream is negotiated in SETUP 2; restart a live session to
     * apply instead of leaving it on the old transport until it dies. */
    if (running_.load()) {
        Stop();
        Start("", ResolveLegacy());
    }
}

/* Speech recognition hands over whatever was said: "1234", "一 二 三 四", or a
 * whole sentence with the digits in it. Keep the digits, ASCII or Chinese. */
bool AirplayService::EnterPin(const std::string& spoken) {
    static const struct {
        const char* word;
        char digit;
    } kDigits[] = {
        {"零", '0'}, {"〇", '0'}, {"洞", '0'}, {"一", '1'}, {"幺", '1'}, {"二", '2'},
        {"两", '2'}, {"三", '3'}, {"四", '4'}, {"五", '5'}, {"六", '6'}, {"七", '7'},
        {"八", '8'}, {"九", '9'},
    };

    std::string digits;
    for (size_t i = 0; i < spoken.size() && digits.size() < 4;) {
        const char c = spoken[i];
        if (c >= '0' && c <= '9') {
            digits += c;
            i++;
            continue;
        }
        bool matched = false;
        for (const auto& entry : kDigits) {
            const size_t len = strlen(entry.word);
            if (spoken.compare(i, len, entry.word) == 0) {
                digits += entry.digit;
                i += len;
                matched = true;
                break;
            }
        }
        if (!matched) {
            i++;
        }
    }
    if (digits.size() != 4) {
        ESP_LOGW(TAG, "\"%s\" does not contain four digits", spoken.c_str());
        return false;
    }
    /* Parsing four digits is not the same as delivering them: without a session
     * parked on a prompt the code goes nowhere, and reporting success would make
     * the assistant tell the user something that did not happen. */
    if (!pin_pending_.load()) {
        ESP_LOGW(TAG, "no session is waiting for a pairing code");
        return false;
    }

    if (pin_semaphore_ == nullptr) {
        pin_semaphore_ = xSemaphoreCreateBinary();
        if (pin_semaphore_ == nullptr) {
            return false;
        }
    }
    memcpy(pending_pin_, digits.c_str(), 4);
    pending_pin_[4] = '\0';
    pin_pending_ = false;
    xSemaphoreGive(pin_semaphore_);
    ESP_LOGI(TAG, "pairing code accepted");
    return true;
}

bool AirplayService::WaitForPin(char pin[5]) {
    if (pin_semaphore_ == nullptr) {
        pin_semaphore_ = xSemaphoreCreateBinary();
        if (pin_semaphore_ == nullptr) {
            return false;
        }
    }
    /* A code left over from an earlier prompt must not be consumed as the answer
     * to this one. */
    while (xSemaphoreTake(pin_semaphore_, 0) == pdTRUE) {
    }
    pin_pending_ = true;
    ESP_LOGW(TAG, "receiver shows a pairing code; ask the user for it");
    /* Poll instead of one long take: a stop request while the code is pending has
     * to end the session, otherwise the device sits wedged for two minutes and
     * refuses every new start. */
    bool arrived = false;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(120000);
    while (xTaskGetTickCount() < deadline) {
        if (xSemaphoreTake(pin_semaphore_, pdMS_TO_TICKS(200)) == pdTRUE) {
            arrived = true;
            break;
        }
        if (stop_requested_.load()) {
            ESP_LOGW(TAG, "stop requested while waiting for the pairing code");
            break;
        }
    }
    pin_pending_ = false;
    if (!arrived) {
        ESP_LOGE(TAG, "no pairing code arrived, giving up");
        pin_timeout_ = true;
        return false;
    }
    memcpy(pin, pending_pin_, 5);
    return true;
}

AirplayService::StartState AirplayService::WaitForStart(int timeout_ms) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        if (pin_pending_.load()) {
            return StartState::kPinRequired;
        }
        if (airplay_is_running()) {
            return StartState::kEstablished;
        }
        if (!running_.load()) {
            return StartState::kFailed;
        }
        if (xTaskGetTickCount() >= deadline) {
            return StartState::kTimeout;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

std::string AirplayService::StatusJson() const {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return "{}";
    }
    cJSON_AddBoolToObject(root, "running", running_.load());
    cJSON_AddBoolToObject(root, "legacy", legacy_);
    cJSON_AddStringToObject(root, "host", host_.c_str());
    /* The receiver this device last mirrored to, session or not. Without it a
     * caller trying to reopen a session has to browse the LAN again and then
     * cannot tell which of many receivers the user meant. */
    cJSON_AddStringToObject(root, "remembered", ResolveHost().c_str());
    cJSON_AddBoolToObject(root, "remembered_legacy", ResolveLegacy());
    cJSON_AddStringToObject(root, "result", last_result_.c_str());
    cJSON_AddBoolToObject(root, "streamed", airplay_last_session_streamed());
    cJSON_AddBoolToObject(root, "pin_required", pin_pending_.load());
    cJSON_AddNumberToObject(root, "backoff_ms", airplay_backoff_ms());
    cJSON_AddBoolToObject(root, "audio", audio_enabled_.load());
    {
        const AirplayAudioSink& sink = AirplayAudioSink::Instance();
        cJSON_AddNumberToObject(root, "audio_frames", (double)sink.pushed_frames());
        cJSON_AddNumberToObject(root, "audio_stalled", (double)sink.stalled_chunks());
    }
    /* What is on the TV: the capture client's screen or this device's UI. */
    cJSON_AddBoolToObject(root, "capture_client", media_net_ready());
    /* Where the capture stream comes from when this device dials out, so the
     * caller can tell "nobody is streaming" from "the stream is unreachable". */
    cJSON_AddStringToObject(root, "pull", media_net_pull_target());
    cJSON_AddStringToObject(root, "source", capture_source_.load() ? "capture"
                             : (display_ != nullptr && display_->webcast_visible())
                                   ? "webcast"
                                   : "display");
    {
        /* Page streaming from the webcaster, so one status answers "is the web
         * page still alive" without its own tool. */
        auto& webcast = WebcastClient::GetInstance();
        cJSON* web = cJSON_CreateObject();
        if (web != nullptr) {
            cJSON_AddBoolToObject(web, "streaming", webcast.streaming());
            cJSON_AddBoolToObject(web, "stalled", webcast.stalled());
            cJSON_AddNumberToObject(web, "frames", (double)webcast.frames());
            cJSON_AddStringToObject(web, "host", webcast.Host().c_str());
            cJSON_AddItemToObject(root, "webcast", web);
        }
    }
    if (display_ != nullptr) {
        cJSON_AddNumberToObject(root, "width", display_->width());
        cJSON_AddNumberToObject(root, "height", display_->height());
        cJSON_AddNumberToObject(root, "fps", display_->fps());
        cJSON_AddNumberToObject(root, "rendered_frames", display_->frame_counter());
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string result = text != nullptr ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return result;
}

/* The LAN list the model can pick from. A cold list gets one blocking browse:
 * answering "nothing here" while receivers that do answer queries are sitting
 * on the network would be a lie. */
std::string AirplayService::DiscoverJson() {
    airplay_target_t targets[AP_DISCOVERY_MAX_TARGETS];
    int count = ap_discovery_snapshot(targets, AP_DISCOVERY_MAX_TARGETS);
    if (count == 0) {
        ap_discovery_browse();
        count = ap_discovery_snapshot(targets, AP_DISCOVERY_MAX_TARGETS);
    }

    cJSON* array = cJSON_CreateArray();
    if (array == nullptr) {
        return "[]";
    }
    for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) {
            break;
        }
        cJSON_AddStringToObject(item, "name", targets[i].name);
        cJSON_AddStringToObject(item, "host", targets[i].host);
        cJSON_AddNumberToObject(item, "port", targets[i].port);
        cJSON_AddBoolToObject(item, "legacy", targets[i].legacy);
        cJSON_AddItemToArray(array, item);
    }
    char* text = cJSON_PrintUnformatted(array);
    std::string result = text != nullptr ? text : "[]";
    cJSON_free(text);
    cJSON_Delete(array);
    return result;
}

bool AirplayService::ShowText(const std::string& text, const DisplayTextStyle& style) {
    if (display_ == nullptr) {
        return false;
    }
    display_->ShowText(text, style);
    return true;
}

size_t AirplayService::ScreenBytes() const {
    if (display_ == nullptr) {
        return 0;
    }
    return static_cast<size_t>(display_->width()) * display_->height() * 2;
}

bool AirplayService::CopyScreen(uint8_t* dst, size_t cap) {
    return display_ != nullptr && display_->CopyFramebuffer(dst, cap);
}

bool AirplayService::EncodeScreenFrame(uint8_t* dst, size_t cap, size_t* len) {
    if (display_ == nullptr || dst == nullptr || len == nullptr) {
        return false;
    }
    const int width = display_->width();
    const int height = display_->height();
    const size_t luma = static_cast<size_t>(width) * height;

    auto* yuv = static_cast<uint8_t*>(heap_caps_aligned_alloc(64, luma * 3 / 2, MALLOC_CAP_SPIRAM));
    auto* annexb = static_cast<uint8_t*>(heap_caps_malloc(kAuCap, MALLOC_CAP_SPIRAM));
    if (yuv == nullptr || annexb == nullptr) {
        heap_caps_free(yuv);
        heap_caps_free(annexb);
        return false;
    }

    display_->LockRender();
    Rgb565ToI420(display_->framebuffer(), width, height, yuv, yuv + luma, yuv + luma + luma / 4);
    display_->UnlockRender();

    /* gop 1 makes every frame an IDR, so the dump decodes on its own. */
    esp_h264_enc_handle_t enc = nullptr;
    bool ok = OpenEncoder(&enc, width, height, display_->fps(), 1);
    if (ok) {
        esp_h264_enc_in_frame_t in = {};
        in.raw_data.buffer = yuv;
        in.raw_data.len = static_cast<uint32_t>(luma * 3 / 2);
        esp_h264_enc_out_frame_t out = {};
        out.raw_data.buffer = annexb;
        out.raw_data.len = static_cast<uint32_t>(kAuCap);
        ok = esp_h264_enc_process(enc, &in, &out) == ESP_H264_ERR_OK && out.length > 0 &&
             out.length <= cap;
        if (ok) {
            memcpy(dst, annexb, out.length);
            *len = out.length;
        }
        esp_h264_enc_close(enc);
        esp_h264_enc_del(enc);
    }

    heap_caps_free(yuv);
    heap_caps_free(annexb);
    return ok;
}

void AirplayService::SessionTask(void* arg) {
    auto* self = static_cast<AirplayService*>(arg);

    hap_identity_t identity;
    if (LoadIdentity(&identity) != ESP_OK) {
        self->last_result_ = "cannot load the HAP identity";
        ESP_LOGE(TAG, "%s", self->last_result_.c_str());
        self->task_ = nullptr;
        self->running_ = false;
        vTaskDelete(nullptr);
        return;
    }

    char local_ip[16];
    GetLocalIp(local_ip, sizeof(local_ip));
    char device_name[48];
    snprintf(device_name, sizeof(device_name), "%s", CONFIG_AIRPLAY_DEVICE_NAME);

    /* A capture client (macOS capture-cli) wins: it streams the real screen,
     * already encoded, so the TV shows that instead of this device's UI. The
     * choice is made per session - a client may connect or drop between them. */
    bool capture = media_net_ready();
    const airplay_live_source_t* live = capture ? media_net_source() : &s_source;
    if (live == nullptr) {
        capture = false;
        live = &s_source;
    }
    self->capture_source_ = capture;

    airplay_config_t cfg = {};
    cfg.host = self->host_.c_str();
    cfg.port = self->port_;
    cfg.device_name = device_name;
    cfg.local_address = local_ip;
    cfg.identity = &identity;
    cfg.live_source = live;
    cfg.frame_interval_ms = 0;
    cfg.request_pin = [](char pin[5], void* user_data) {
        return static_cast<AirplayService*>(user_data)->WaitForPin(pin);
    };
    cfg.user_data = self;

    if (self->legacy_) {
        cfg.legacy = true;
        cfg.legacy_data_mode = 1;
        cfg.legacy_key_mode = 0;
        cfg.legacy_iv_source = 0;
        cfg.legacy_inband_sps = true;
        cfg.legacy_pairing = false;
    }

    ESP_LOGI(TAG, "session start: host %s, local %s, source %s%s", cfg.host, local_ip,
             capture ? "capture stream" : "virtual display",
             capture && !self->legacy_ ? " (audio needs the legacy path)" : "");
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0;; attempt++) {
        /* One audio producer per session, picked by what is on screen: a capture
         * client streams its own frames (the Mac's sound), the virtual display
         * feeds the sink with the assistant's voice. The sink arms per attempt:
         * a dead session must not leave it accepting audio with nobody pulling,
         * which would stall every chunk for the stall timeout before falling
         * back to the local speaker. */
        AirplayAudioSink& sink = AirplayAudioSink::Instance();
        if (self->legacy_ && capture && self->audio_enabled_.load()) {
            sink.StopForSession();
            cfg.legacy_audio = true;
            cfg.legacy_audio_spf = kAudioSpf;
            cfg.legacy_audio_frame = CaptureAudioFrame;
            cfg.legacy_audio_ctx = nullptr;
        } else if (self->legacy_ && self->audio_enabled_.load()) {
            sink.StartForSession(Board::GetInstance().GetAudioCodec()->output_sample_rate());
            cfg.legacy_audio = sink.live();
            cfg.legacy_audio_spf = kAudioSpf;
            cfg.legacy_audio_frame = SinkAudioFrame;
            cfg.legacy_audio_ctx = &sink;
        } else {
            sink.StopForSession();
            cfg.legacy_audio = false;
            cfg.legacy_audio_frame = nullptr;
            cfg.legacy_audio_ctx = nullptr;
        }
        err = airplay_mirror(&cfg);
        sink.StopForSession();
        self->last_result_ = err == ESP_OK ? "session ended" : esp_err_to_name(err);
        ESP_LOGI(TAG, "session end: %s (streamed=%d)", self->last_result_.c_str(),
                 (int)airplay_last_session_streamed());
        /* A refused session is usually the receiver still tearing the previous
         * one down right after this device rebooted; retry with growing gaps.
         * A clean end (stop request, receiver goodbye), a pairing prompt nobody
         * answered, or the last attempt are final. */
        if (err == ESP_OK || attempt >= 4 || self->stop_requested_.load() ||
            self->pin_timeout_.load()) {
            break;
        }
        const uint32_t delay_ms = 5000u << attempt;
        ESP_LOGW(TAG, "receiver not ready, retrying in %lu ms", (unsigned long)delay_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    if (airplay_last_session_streamed()) {
        /* Only a target that actually put frames on a screen is worth keeping:
         * Start() used to store it before dialling, so a typo or a wrong port
         * overwrote the receiver every caller falls back to. */
        self->Remember(self->host_ + ":" + std::to_string(self->port_), self->legacy_);
        /* Receivers behind a network that swallows their multicast, or that only
         * announce once per power cycle, never answer a browse: frames actually
         * reaching a screen is the only evidence the list will ever get. */
        ap_discovery_remember(self->name_.empty() ? self->host_.c_str() : self->name_.c_str(),
                              self->host_.c_str(), self->port_, self->legacy_);
    }

    self->task_ = nullptr;
    self->running_ = false;
    vTaskDelete(nullptr);
}

void AirplayService::StartNetworkServices() {
    xTaskCreate(NetworkTask, "airplay_net", 4 * 1024, this, 2, nullptr);
}

void AirplayService::CaptureWatchTask(void* arg) {
    (void)arg;
    bool was_ready = media_net_ready();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        const bool ready = media_net_ready();
        if (ready == was_ready) {
            continue;
        }
        was_ready = ready;
        auto& self = GetInstance();
        /* The source is fixed when a session starts, so a client that arrives
         * mid-session would stay invisible; a client that leaves would freeze
         * the TV on its last frame. Handing over takes a new session. */
        if (!self.running_.load() || self.capture_source_.load() == ready) {
            continue;
        }
        ESP_LOGI(TAG, "capture client %s: restarting the session on the %s",
                 ready ? "connected" : "left", ready ? "capture stream" : "virtual display");
        self.Stop();
        for (int i = 0; i < 150 && self.running_.load(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        /* The receiver needs a moment to tear the old session down; reconnecting
         * straight away lands on a black screen. */
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (!self.Start("", false)) {
            ESP_LOGW(TAG, "cannot restart the session on the %s",
                     ready ? "capture stream" : "virtual display");
        }
    }
}

void AirplayService::NetworkTask(void* arg) {
    (void)arg;
    char ip[16] = {0};
    for (int i = 0; i < 120; i++) {
        GetLocalIp(ip, sizeof(ip));
        if (strcmp(ip, "0.0.0.0") != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (strcmp(ip, "0.0.0.0") == 0) {
        ESP_LOGW(TAG, "no network address, bench API and receiver discovery stay off");
        vTaskDelete(nullptr);
        return;
    }

    /* The bench HTTP server binds sockets, so it can only come up after the
     * TCP/IP stack; the board constructor runs before that. */
    AirplayHttpStart();

    /* A station that dozes between beacons misses the announcements that
     * receivers which never answer a query (the projector is one) rely on. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (ap_discovery_start() != ESP_OK) {
        ESP_LOGW(TAG, "receiver discovery unavailable; self.airplay.discover stays empty");
    }

    /* A screen already encoded on the network beats anything this chip can
     * render: the capture client (macOS capture-cli) sends the real desktop. */
    if (media_net_init(CONFIG_AIRPLAY_VIDEO_INPUT_PORT) && media_net_start()) {
        ESP_LOGI(TAG, "capture stream accepted on port %d (audio on %d)",
                 CONFIG_AIRPLAY_VIDEO_INPUT_PORT, CONFIG_AIRPLAY_VIDEO_INPUT_PORT + 1);
        xTaskCreate(CaptureWatchTask, "airplay_capture", 3 * 1024, nullptr, 2, nullptr);
    } else {
        ESP_LOGW(TAG, "capture stream listener unavailable; mirroring this device's UI only");
    }

    ESP_LOGI(TAG, "network is up (%s); mirroring starts when self.airplay.start is called", ip);
    vTaskDelete(nullptr);
}
