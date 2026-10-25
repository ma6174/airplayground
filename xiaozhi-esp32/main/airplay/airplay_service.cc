#include "airplay_service.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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
#include "lvgl_display/lvgl_image.h"
#include "settings.h"
#include "webcast_client.h"

#if SOC_JPEG_DECODE_SUPPORTED
#include "driver/jpeg_decode.h"
#endif

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

/* ---------- pictures ---------- */

/* Download and body caps: 4 MB covers photo-sized JPEGs without threatening the
 * PSRAM budget. Decoding is capped separately: the decoder hands out 2 bytes per
 * pixel, so the cap keeps the transient frame around 4 MB. */
constexpr size_t kMaxImageBytes = 4u * 1024 * 1024;
constexpr uint32_t kMaxImagePixels = 2100000; /* 1920x1080 fits. */

/* `required_type`, when set, is the response content type the caller needs: the
 * body is only pulled when the server says it is sending that, so an answer in
 * the wrong format costs one round trip instead of a megabyte of download. */
bool DownloadUrl(const std::string& url, size_t cap, std::vector<uint8_t>* out, std::string* error,
                 const char* required_type = nullptr) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    /* 20 s between chunks: this is a per-read wait, and a long-haul HTTPS download
     * routinely goes quiet for ten seconds mid-body, which used to kill the fetch. */
    http->SetTimeout(20000);
    if (auto opened = http->Open("GET", url); !opened) {
        *error = "cannot open " + url + ": " + opened.error().ToString();
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        *error = "no answer from " + url;
        http->Close();
        return false;
    }
    if (*status != 200) {
        *error = "HTTP " + std::to_string(*status) + " for " + url;
        http->Close();
        return false;
    }
    /* Open waits for the response headers, so the type is known before the body
     * is streamed. A server that sends no type is given the benefit of the doubt:
     * the decoder turns the body down if it really is not a JPEG. */
    if (required_type != nullptr) {
        const std::string type = http->GetResponseHeader("Content-Type");
        if (!type.empty() && type.rfind(required_type, 0) != 0) {
            *error = "the server answered with " + type + " instead of a JPEG";
            http->Close();
            return false;
        }
    }
    out->clear();
    char chunk[4096];
    for (;;) {
        auto read = http->Read(chunk, sizeof(chunk));
        if (!read) {
            *error = "download failed: " + read.error().ToString();
            http->Close();
            return false;
        }
        if (*read == 0) {
            break;
        }
        if (out->size() + static_cast<size_t>(*read) > cap) {
            *error = "the picture is larger than the 4 MB limit";
            http->Close();
            return false;
        }
        out->insert(out->end(), chunk, chunk + *read);
    }
    http->Close();
    if (out->empty()) {
        *error = "the server sent no body: " + url;
        return false;
    }
    return true;
}

/* Reads the picture size from the SOF marker, before anything allocates. */
bool JpegSize(const uint8_t* d, size_t n, int* w, int* h) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) {
        return false;
    }
    size_t i = 2;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) {
            i++;
            continue;
        }
        const uint8_t marker = d[i + 1];
        if (marker == 0xFF) {
            i++;
            continue;
        }
        /* Standalone markers carry no length field. */
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
            i += 2;
            continue;
        }
        const size_t seg = (static_cast<size_t>(d[i + 2]) << 8) | d[i + 3];
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (i + 9 > n) {
                return false;
            }
            *h = (d[i + 5] << 8) | d[i + 6];
            *w = (d[i + 7] << 8) | d[i + 8];
            return *w > 0 && *h > 0;
        }
        i += 2 + seg;
    }
    return false;
}

/* Resamples an RGB565 picture to the largest size that fits the screen with the
 * aspect ratio kept (area average); a picture that already fits is taken 1:1. */
bool FitToScreen(const uint8_t* src, int sw, int sh, int stride, int screen_w, int screen_h,
                 uint8_t** out, int* dw, int* dh) {
    if (sw <= screen_w && sh <= screen_h) {
        *dw = sw;
        *dh = sh;
        *out = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(sw) * sh * 2,
                                                      MALLOC_CAP_SPIRAM));
        if (*out == nullptr) {
            return false;
        }
        for (int y = 0; y < sh; y++) {
            memcpy(*out + static_cast<size_t>(y) * sw * 2, src + static_cast<size_t>(y) * stride,
                   static_cast<size_t>(sw) * 2);
        }
        return true;
    }

    /* Aspect-true fit: scale = min(screen_w/sw, screen_h/sh), compared without
     * floating point. */
    if (static_cast<int64_t>(screen_w) * sh < static_cast<int64_t>(screen_h) * sw) {
        *dw = screen_w;
        *dh = std::max(1, static_cast<int>(static_cast<int64_t>(sh) * screen_w / sw));
    } else {
        *dh = screen_h;
        *dw = std::max(1, static_cast<int>(static_cast<int64_t>(sw) * screen_h / sh));
    }

    *out = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(*dw) * *dh * 2,
                                                  MALLOC_CAP_SPIRAM));
    if (*out == nullptr) {
        return false;
    }
    for (int y = 0; y < *dh; y++) {
        const int sy0 = static_cast<int>(static_cast<int64_t>(y) * sh / *dh);
        int sy1 = static_cast<int>(static_cast<int64_t>(y + 1) * sh / *dh);
        if (sy1 <= sy0) {
            sy1 = sy0 + 1;
        }
        auto* dst_row = reinterpret_cast<uint16_t*>(*out + static_cast<size_t>(y) * *dw * 2);
        for (int x = 0; x < *dw; x++) {
            const int sx0 = static_cast<int>(static_cast<int64_t>(x) * sw / *dw);
            int sx1 = static_cast<int>(static_cast<int64_t>(x + 1) * sw / *dw);
            if (sx1 <= sx0) {
                sx1 = sx0 + 1;
            }
            uint32_t r = 0;
            uint32_t g = 0;
            uint32_t b = 0;
            uint32_t count = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const auto* row = reinterpret_cast<const uint16_t*>(src + static_cast<size_t>(sy) * stride);
                for (int sx = sx0; sx < sx1; sx++) {
                    const uint16_t p = row[sx];
                    r += (p >> 11) & 0x1F;
                    g += (p >> 5) & 0x3F;
                    b += p & 0x1F;
                    count++;
                }
            }
            dst_row[x] = static_cast<uint16_t>((r / count) << 11 | (g / count) << 5 | (b / count));
        }
    }
    return true;
}

#if SOC_JPEG_DECODE_SUPPORTED
/* The chip decodes JPEG in hardware; calling the driver directly keeps the
 * 130 KB software codec (esp_new_jpeg) out of the image and is faster too.
 * Baseline JPEG only: the engine rejects progressive files. */
bool ShowJpegOnDisplay(VirtualDisplay* display, const uint8_t* jpeg, size_t len, std::string* error) {
    int w = 0;
    int h = 0;
    if (!JpegSize(jpeg, len, &w, &h)) {
        *error = "the data is not a JPEG picture";
        return false;
    }
    if (static_cast<uint32_t>(w) * h > kMaxImagePixels) {
        *error = "the picture is " + std::to_string(w) + "x" + std::to_string(h) +
                 ", above the 2 megapixel decode limit; resize it first";
        return false;
    }

    jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 1,
        .timeout_ms = 2000,
    };
    jpeg_decoder_handle_t decoder = nullptr;
    if (jpeg_new_decoder_engine(&engine_cfg, &decoder) != ESP_OK) {
        *error = "cannot start the JPEG engine";
        return false;
    }

    jpeg_decode_memory_alloc_cfg_t tx_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t tx_size = 0;
    auto* bit_stream = static_cast<uint8_t*>(jpeg_alloc_decoder_mem(len, &tx_cfg, &tx_size));
    jpeg_decode_memory_alloc_cfg_t rx_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    memcpy(bit_stream, jpeg, len);

    /* Ask the engine what the file really is before sizing the output: 420/422
     * come out padded to whole MCUs (16 px), 444 and gray come out unpadded. */
    jpeg_decode_picture_info_t info = {};
    if (jpeg_decoder_get_info(bit_stream, len, &info) != ESP_OK) {
        heap_caps_free(bit_stream);
        jpeg_del_decoder_engine(decoder);
        *error = "not a supported (baseline) JPEG";
        return false;
    }
    const size_t out_w = info.width;
    const size_t out_h = info.height;
    /* Where the engine puts a picture is not the same for every sampling: 4:2:0
     * and 4:2:2 come back padded to whole 16 px MCUs, 4:4:4 and grayscale do
     * not. Sizing the buffer for the padded case covers all of them (it is the
     * upper bound), and the stride is taken from what the engine reports it
     * wrote. */
    const size_t padded_w = (out_w + 15) & ~15u;
    const size_t padded_h = (out_h + 15) & ~15u;
    const size_t plane_len = out_w * out_h * 2;
    const size_t padded_len = padded_w * padded_h * 2;
    bool gray = false;
    switch (info.sample_method) {
        case JPEG_DOWN_SAMPLING_GRAY:
            gray = true;
            break;
        case JPEG_DOWN_SAMPLING_YUV444:
        case JPEG_DOWN_SAMPLING_YUV422:
        case JPEG_DOWN_SAMPLING_YUV420:
            break;
        default:
            heap_caps_free(bit_stream);
            jpeg_del_decoder_engine(decoder);
            *error = "the JPEG sampling is not supported";
            return false;
    }
    size_t rx_size = 0;
    auto* out_buf = static_cast<uint8_t*>(jpeg_alloc_decoder_mem(padded_len, &rx_cfg, &rx_size));
    if (out_buf == nullptr || rx_size < padded_len) {
        heap_caps_free(bit_stream);
        jpeg_del_decoder_engine(decoder);
        *error = "no memory for the JPEG decode";
        return false;
    }

    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };
    uint32_t produced = 0;
    const esp_err_t process_err =
        jpeg_decoder_process(decoder, &decode_cfg, bit_stream, len, out_buf, padded_len, &produced);
    jpeg_del_decoder_engine(decoder);
    heap_caps_free(bit_stream);
    if (process_err != ESP_OK || produced == 0) {
        ESP_LOGE(TAG, "JPEG decode failed: err=%d sampling=%d %zux%zu produced=%u",
                 (int)process_err, (int)info.sample_method, out_w, out_h, (unsigned)produced);
        heap_caps_free(out_buf);
        *error = "JPEG decode failed";
        return false;
    }
    int stride = 0;
    if (gray && produced == out_w * out_h) {
        stride = static_cast<int>(out_w);
    } else if (produced == padded_len) {
        stride = static_cast<int>(padded_w * 2);
    } else if (produced == plane_len) {
        stride = static_cast<int>(out_w * 2);
    } else {
        ESP_LOGE(TAG, "JPEG decode produced %u bytes, expected %u or %u", (unsigned)produced,
                 (unsigned)plane_len, (unsigned)padded_len);
        heap_caps_free(out_buf);
        *error = "JPEG decode produced an unexpected size";
        return false;
    }

    uint8_t* pixels = out_buf;
    if (gray) {
        /* The engine hands grayscale out as one byte per pixel; expand it to
         * RGB565 in place, from the back so nothing is clobbered. */
        const size_t count = out_w * out_h;
        for (size_t i = count; i > 0; i--) {
            const uint8_t gray_value = pixels[i - 1];
            const auto r5 = static_cast<uint16_t>(gray_value >> 3);
            const auto g6 = static_cast<uint16_t>(gray_value >> 2);
            const uint16_t rgb565 = static_cast<uint16_t>((r5 << 11) | (g6 << 5) | r5);
            pixels[2 * (i - 1) + 1] = static_cast<uint8_t>(rgb565 >> 8);
            pixels[2 * (i - 1)] = static_cast<uint8_t>(rgb565);
        }
    }

    uint8_t* fitted = nullptr;
    int fw = 0;
    int fh = 0;
    const bool ok = FitToScreen(pixels, w, h, stride, display->width(), display->height(), &fitted,
                                &fw, &fh);
    heap_caps_free(out_buf);
    if (!ok) {
        *error = "no memory for the picture";
        return false;
    }

    auto image = std::make_unique<LvglAllocatedImage>(fitted, static_cast<size_t>(fw) * fh * 2, fw,
                                                      fh, fw * 2, LV_COLOR_FORMAT_RGB565);
    if (!image->IsValid()) {
        *error = "the decoded picture is unusable";
        return false;
    }
    display->ShowImage(std::move(image));
    ESP_LOGI(TAG, "picture on screen: %dx%d -> %dx%d", w, h, fw, fh);
    return true;
}
#else
bool ShowJpegOnDisplay(VirtualDisplay* display, const uint8_t* jpeg, size_t len, std::string* error) {
    (void)display;
    (void)jpeg;
    (void)len;
    *error = "this board cannot decode JPEG";
    return false;
}
#endif

/* The legacy photo protocol is one HTTP PUT of the JPEG bytes to /photo on the
 * receiver's AirTunes port, with no mirroring session involved. */
std::string PutPhoto(const std::string& host, uint16_t port, const uint8_t* jpeg, size_t len,
                     bool* accepted) {
    const std::string target = "http://" + host + ":" + std::to_string(port) + "/photo";
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    http->SetTimeout(10000);
    http->SetHeader("Content-Type", "image/jpeg");
    http->SetHeader("X-Apple-HDCPStatus", "No HDCP");
    http->SetContent(std::string(reinterpret_cast<const char*>(jpeg), len));
    if (auto opened = http->Open("PUT", target); !opened) {
        *accepted = false;
        return "cannot reach " + target + ": " + opened.error().ToString();
    }
    auto status = http->GetStatusCode();
    http->Close();
    if (!status) {
        *accepted = false;
        return "the receiver sent no HTTP answer to " + target;
    }
    *accepted = *status == 200;
    if (*accepted) {
        return "receiver accepted the photo; check the TV to confirm it is showing";
    }
    return "receiver refused the photo: HTTP " + std::to_string(*status);
}

}  // namespace

/* One picture job: what was asked, the payload, and how it ended. Lives on the
 * heap; the worker frees it after the grace delay (see ImageTask). */
struct AirplayService::ImageJob {
    enum class Kind { kShowUrl, kShowJpeg, kPhoto };
    Kind kind = Kind::kShowUrl;
    std::string url;
    std::string host;            /* kPhoto: receiver expression. */
    std::vector<uint8_t> jpeg;   /* kShowJpeg input; kPhoto payload. */
    bool ok = false;
    std::string result;
    SemaphoreHandle_t done = nullptr;
};

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

/* Turns a receiver expression into host + port + transport. An empty expression
 * falls back to the remembered receiver (with its transport); anything that is
 * not an address is looked up in the discovery list, which also decides the
 * transport: AppleTV3,2-style boxes only speak legacy AirTunes. */
bool AirplayService::ResolveReceiver(const std::string& expr, std::string* host, uint16_t* port,
                                     bool* legacy, std::string* name) const {
    std::string target = expr;
    if (target.empty()) {
        target = ResolveHost();
        /* Asked to reuse the last receiver: it was remembered together with the
         * way it has to be spoken to (a projector only answers legacy AirTunes). */
        *legacy = *legacy || ResolveLegacy();
    }
    name->clear();
    if (!target.empty() && !LooksLikeAddress(target)) {
        airplay_target_t found = {};
        if (!ap_discovery_find(target.c_str(), &found)) {
            ESP_LOGE(TAG, "no receiver named \"%s\"; self.airplay.discover lists them",
                     target.c_str());
            return false;
        }
        *name = found.name;
        *legacy = *legacy || found.legacy;
        target = std::string(found.host) + ":" + std::to_string(found.port);
        ESP_LOGI(TAG, "\"%s\" resolved to %s (%s)", found.name, target.c_str(),
                 found.legacy ? "legacy AirTunes" : "HAP");
    }
    if (target.empty()) {
        ESP_LOGE(TAG, "no receiver address; pass one in or set CONFIG_AIRPLAY_DEFAULT_HOST");
        return false;
    }

    /* "host:port" for receivers whose RTSP does not live on 7001/7000; the
     * projector advertises 7001, the real RTSP service answers on 7002. */
    uint16_t parsed_port = 7000;
    const auto colon = target.rfind(':');
    if (colon != std::string::npos && target.find(':') == colon) {
        int parsed = atoi(target.c_str() + colon + 1);
        if (parsed > 0 && parsed < 65536) {
            parsed_port = static_cast<uint16_t>(parsed);
            target.resize(colon);
        }
    }
    *host = target;
    *port = parsed_port;
    return true;
}

bool AirplayService::Start(const std::string& host, bool legacy) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "a session is already running");
        return false;
    }

    std::string resolved_host;
    uint16_t port = 7000;
    if (!ResolveReceiver(host, &resolved_host, &port, &legacy, &name_)) {
        running_ = false;
        return false;
    }

    host_ = resolved_host;
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

/* One-shot worker for the picture jobs. Downloading, decoding and the LVGL
 * update need real stack; the callers (MCP main loop, httpd handler) cannot be
 * trusted to have it. */
void AirplayService::ImageTask(void* arg) {
    auto* job = static_cast<ImageJob*>(arg);
    auto* self = &AirplayService::GetInstance();

    switch (job->kind) {
        case ImageJob::Kind::kShowUrl: {
            std::string error;
            std::vector<uint8_t> jpeg;
            /* The type is checked before the body is pulled: a link that points
             * at a PNG or a GIF fails with that in the answer instead of costing
             * a megabyte of download first. Servers that send no type are tried
             * anyway; the decoder has the last word. */
            if (!DownloadUrl(job->url, kMaxImageBytes, &jpeg, &error, "image/jpeg")) {
                job->result = std::move(error);
                break;
            }
            if (!ShowJpegOnDisplay(self->display_, jpeg.data(), jpeg.size(), &error)) {
                job->result = std::move(error);
                break;
            }
            job->ok = true;
            break;
        }
        case ImageJob::Kind::kShowJpeg: {
            std::string error;
            if (!ShowJpegOnDisplay(self->display_, job->jpeg.data(), job->jpeg.size(), &error)) {
                job->result = std::move(error);
                break;
            }
            job->ok = true;
            break;
        }
        case ImageJob::Kind::kPhoto: {
            std::string error;
            if (job->jpeg.empty()) {
                if (!DownloadUrl(job->url, kMaxImageBytes, &job->jpeg, &error)) {
                    job->result = std::move(error);
                    break;
                }
            }
            std::string host;
            uint16_t port = 7000;
            bool legacy = false;
            std::string name;
            if (!self->ResolveReceiver(job->host, &host, &port, &legacy, &name)) {
                job->result = "no receiver by that name (self.airplay.discover lists them)";
                break;
            }
            job->result = PutPhoto(host, port, job->jpeg.data(), job->jpeg.size(), &job->ok);
            break;
        }
    }

    if (job->done != nullptr) {
        xSemaphoreGive(job->done);
    }
    /* The caller copies the result right after the semaphore turns; the delay
     * covers that, and frees the job here when nobody waited for it. */
    vTaskDelay(pdMS_TO_TICKS(10000));
    vSemaphoreDelete(job->done);
    delete job;
    vTaskDelete(nullptr);
}

bool AirplayService::RunImageJob(std::unique_ptr<ImageJob> job, std::string* result) {
    job->done = xSemaphoreCreateBinary();
    if (job->done == nullptr) {
        *result = "no memory";
        return false;
    }
    ImageJob* raw = job.release();
    if (xTaskCreate(ImageTask, "airplay_img", 16 * 1024, raw, 3, nullptr) != pdPASS) {
        vSemaphoreDelete(raw->done);
        delete raw;
        *result = "cannot create the picture task";
        return false;
    }
    /* A download over a long-haul link plus the decode; past that the attempt is
     * a failure and the worker cleans the job up on its own. */
    if (xSemaphoreTake(raw->done, pdMS_TO_TICKS(45000)) != pdTRUE) {
        *result = "timed out after 45 s";
        return false;
    }
    *result = raw->result;
    return raw->ok;
}

bool AirplayService::ShowImageFromUrl(const std::string& url, std::string* error) {
    if (display_ == nullptr) {
        if (error != nullptr) {
            *error = "no screen";
        }
        return false;
    }
    if (url.empty()) {
        display_->ShowImage(nullptr);
        return true;
    }
    auto job = std::make_unique<ImageJob>();
    job->kind = ImageJob::Kind::kShowUrl;
    job->url = url;
    std::string result;
    const bool ok = RunImageJob(std::move(job), &result);
    if (!ok) {
        ESP_LOGE(TAG, "show image: %s", result.c_str());
        if (error != nullptr) {
            *error = result;
        }
    }
    return ok;
}

bool AirplayService::ShowImageFromJpeg(const uint8_t* data, size_t len, std::string* error) {
    if (display_ == nullptr || data == nullptr || len == 0) {
        if (error != nullptr) {
            *error = "no screen or no data";
        }
        return false;
    }
    auto job = std::make_unique<ImageJob>();
    job->kind = ImageJob::Kind::kShowJpeg;
    job->jpeg.assign(data, data + len);
    std::string result;
    const bool ok = RunImageJob(std::move(job), &result);
    if (!ok) {
        ESP_LOGE(TAG, "show image: %s", result.c_str());
        if (error != nullptr) {
            *error = result;
        }
    }
    return ok;
}

std::string AirplayService::SendPhoto(const std::string& url, const std::string& host) {
    auto job = std::make_unique<ImageJob>();
    job->kind = ImageJob::Kind::kPhoto;
    job->url = url;
    job->host = host;
    std::string result;
    if (!RunImageJob(std::move(job), &result)) {
        return "photo failed: " + result;
    }
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
        /* Keep the receiver's advertised name rather than its address: DHCP
         * moves the address, and the name still resolves through the discovery
         * list, so "the TV used last time" keeps working after a renumbering. */
        std::string remembered = self->host_ + ":" + std::to_string(self->port_);
        if (!self->name_.empty()) {
            remembered = self->name_;
        } else {
            airplay_target_t targets[AP_DISCOVERY_MAX_TARGETS];
            const int found = ap_discovery_snapshot(targets, AP_DISCOVERY_MAX_TARGETS);
            for (int i = 0; i < found; i++) {
                if (strcmp(targets[i].host, self->host_.c_str()) == 0 && targets[i].name[0] != '\0') {
                    remembered = targets[i].name;
                    break;
                }
            }
        }
        self->Remember(remembered, self->legacy_);
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
