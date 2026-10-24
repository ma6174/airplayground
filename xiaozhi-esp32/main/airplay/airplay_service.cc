#include "airplay_service.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs.h>

#include "board.h"
#include "cJSON.h"
#include "display/virtual_display.h"
#include "settings.h"

#include "esp_h264_enc_param.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_sw.h"
#include "esp_h264_types.h"

extern "C" {
#include "airplay.h"
#include "media_avcc.h"
}

#define TAG "Airplay"

namespace {

constexpr size_t kAuCap = 128 * 1024;
constexpr size_t kAvccCap = 64;
constexpr uint8_t kGop = 15;
constexpr uint32_t kBitrateBps = 1200000;
constexpr uint8_t kQpMin = 26;
constexpr uint8_t kQpMax = 42;

/* One screen-to-H.264 pipeline. It lives for the whole session: the protocol
 * stack asks for the decoder configuration record once and then for one access
 * unit per frame tick. */
struct Sink {
    VirtualDisplay* display = nullptr;
    esp_h264_enc_handle_t enc = nullptr;
    int width = 0;
    int height = 0;
    int fps = 15;
    uint8_t* i420 = nullptr;
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

/* RGB565 -> planar YUV420 (BT.601 studio swing, integer only). */
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

bool OpenEncoder(int width, int height, int fps) {
    esp_h264_enc_cfg_sw_t cfg = {};
    cfg.pic_type = ESP_H264_RAW_FMT_I420;
    cfg.gop = kGop;
    cfg.fps = static_cast<uint8_t>(fps);
    cfg.res.width = static_cast<uint16_t>(width);
    cfg.res.height = static_cast<uint16_t>(height);
    cfg.rc.bitrate = kBitrateBps;
    cfg.rc.qp_min = kQpMin;
    cfg.rc.qp_max = kQpMax;

    esp_h264_err_t err = esp_h264_enc_sw_new(&cfg, &s_sink.enc);
    if (err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "cannot create the encoder (%d)", err);
        s_sink.enc = nullptr;
        return false;
    }
    err = esp_h264_enc_open(s_sink.enc);
    if (err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "cannot open the encoder (%d)", err);
        esp_h264_enc_del(s_sink.enc);
        s_sink.enc = nullptr;
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

    Rgb565ToI420(s_sink.display->framebuffer(), width, height, s_sink.i420, s_sink.i420 + luma,
                 s_sink.i420 + luma + luma / 4);

    esp_h264_enc_in_frame_t in = {};
    in.raw_data.buffer = s_sink.i420;
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
    if (s_sink.i420 == nullptr) {
        s_sink.i420 = static_cast<uint8_t*>(heap_caps_malloc(luma * 3 / 2, MALLOC_CAP_SPIRAM));
        s_sink.annexb = static_cast<uint8_t*>(heap_caps_malloc(kAuCap, MALLOC_CAP_SPIRAM));
        s_sink.au = static_cast<uint8_t*>(heap_caps_malloc(kAuCap, MALLOC_CAP_SPIRAM));
    }
    if (s_sink.i420 == nullptr || s_sink.annexb == nullptr || s_sink.au == nullptr) {
        ESP_LOGE(TAG, "no PSRAM for the encoder buffers (%u bytes luma)", (unsigned)luma);
        return nullptr;
    }

    CloseEncoder();
    s_sink.have_pending = false;
    s_sink.au_valid = false;
    s_sink.frame_index = 0;
    s_sink.pts_ms = 0;

    if (!OpenEncoder(s_sink.width, s_sink.height, s_sink.fps)) {
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

void AirplayService::RememberHost(const std::string& host) const {
    Settings settings("airplay", true);
    settings.SetString("host", host);
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
    }
    if (target.empty()) {
        ESP_LOGE(TAG, "no receiver address; pass one in or set CONFIG_AIRPLAY_DEFAULT_HOST");
        running_ = false;
        return false;
    }

    host_ = target;
    legacy_ = legacy;
    RememberHost(target);
    last_result_ = "starting";

    BaseType_t created = xTaskCreate(SessionTask, "airplay", 16 * 1024, this, 3, &task_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "cannot create the session task");
        task_ = nullptr;
        running_ = false;
        return false;
    }
    ESP_LOGI(TAG, "connecting to %s (%s)", host_.c_str(), legacy_ ? "legacy AirTunes" : "HAP");
    return true;
}

void AirplayService::Stop() {
    if (!running_.load()) {
        return;
    }
    ESP_LOGI(TAG, "stop requested");
    airplay_request_stop();
}

bool AirplayService::EnterPin(const std::string& pin) {
    if (pin.size() != 4) {
        return false;
    }
    if (pin_semaphore_ == nullptr) {
        pin_semaphore_ = xSemaphoreCreateBinary();
        if (pin_semaphore_ == nullptr) {
            return false;
        }
    }
    memcpy(pending_pin_, pin.c_str(), 4);
    pending_pin_[4] = '\0';
    xSemaphoreGive(pin_semaphore_);
    return true;
}

bool AirplayService::WaitForPin(char pin[5]) {
    if (pin_semaphore_ == nullptr) {
        pin_semaphore_ = xSemaphoreCreateBinary();
        if (pin_semaphore_ == nullptr) {
            return false;
        }
    }
    ESP_LOGW(TAG, "receiver asks for a pairing code; call self.airplay.enter_pin");
    if (xSemaphoreTake(pin_semaphore_, pdMS_TO_TICKS(120000)) != pdTRUE) {
        ESP_LOGE(TAG, "no pairing code arrived, giving up");
        return false;
    }
    memcpy(pin, pending_pin_, 5);
    return true;
}

std::string AirplayService::StatusJson() const {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return "{}";
    }
    cJSON_AddBoolToObject(root, "running", running_.load());
    cJSON_AddBoolToObject(root, "legacy", legacy_);
    cJSON_AddStringToObject(root, "host", host_.c_str());
    cJSON_AddStringToObject(root, "result", last_result_.c_str());
    cJSON_AddBoolToObject(root, "streamed", airplay_last_session_streamed());
    cJSON_AddNumberToObject(root, "backoff_ms", airplay_backoff_ms());
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

    airplay_config_t cfg = {};
    cfg.host = self->host_.c_str();
    cfg.port = 7000;
    cfg.device_name = device_name;
    cfg.local_address = local_ip;
    cfg.identity = &identity;
    cfg.live_source = &s_source;
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
        cfg.legacy_audio = false;
    }

    ESP_LOGI(TAG, "session start: host %s, local %s, %dx%d", cfg.host, local_ip, s_sink.width,
             s_sink.height);
    esp_err_t err = airplay_mirror(&cfg);
    self->last_result_ = err == ESP_OK ? "session ended" : esp_err_to_name(err);
    ESP_LOGI(TAG, "session end: %s (streamed=%d)", self->last_result_.c_str(),
             (int)airplay_last_session_streamed());

    self->task_ = nullptr;
    self->running_ = false;
    vTaskDelete(nullptr);
}

void AirplayService::AutoStart() {
    if (ResolveHost().empty()) {
        ESP_LOGI(TAG, "no default receiver configured, waiting for self.airplay.start");
        return;
    }
    xTaskCreate(AutoStartTask, "airplay_auto", 4 * 1024, this, 2, nullptr);
}

void AirplayService::AutoStartTask(void* arg) {
    auto* self = static_cast<AirplayService*>(arg);

    char ip[16] = {0};
    for (int i = 0; i < 120; i++) {
        GetLocalIp(ip, sizeof(ip));
        if (strcmp(ip, "0.0.0.0") != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (strcmp(ip, "0.0.0.0") == 0) {
        ESP_LOGW(TAG, "no network address, skipping the automatic session");
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "network is up (%s), connecting to the configured receiver", ip);
    self->Start("", false);
    vTaskDelete(nullptr);
}
