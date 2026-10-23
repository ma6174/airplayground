#include "media_live.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_h264_enc_param.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_sw.h"
#include "esp_h264_types.h"
#include "esp_log.h"

#include "airplay.h"
#include "media_avcc.h"
#include "media_pattern.h"

static const char *TAG = "media";

/* The codec record and one IDR access unit never coexist with a second copy,
 * so one buffer each, sized once. */
#define LIVE_AU_CAP (64u * 1024u)
#define LIVE_GOP 60
#define LIVE_BITRATE_BPS 1000000
#define LIVE_QP_MIN 26
#define LIVE_QP_MAX 42
/* The AVCDecoderConfigurationRecord for one SPS/PPS pair stays far below the
 * buffer; the cap just turns a runaway SPS into a clean failure. */
#define LIVE_AVCC_CAP 64

static struct {
    bool ready;
    esp_h264_enc_handle_t enc;
    uint8_t *yuv;     /* pattern render target, also the encoder input */
    uint8_t *annexb;  /* encoder output before re-framing */
    uint8_t *au;      /* AVCC-framed access unit handed to the session */
    uint8_t avcc[LIVE_AVCC_CAP];
    size_t avcc_len;
    /* The epoch's first frame is encoded eagerly (its SPS/PPS feed the codec
     * record) and parked in the delivery buffer until the session asks for
     * frame one. */
    bool have_pending;
    size_t pending_len;
    uint32_t frame_index;
    uint32_t pts_ms;
} s_live;

static size_t yuv_len(void)
{
    return MEDIA_PATTERN_YUV_LEN;
}

static void *big_alloc(size_t len)
{
    void *block = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (block != NULL) {
        return block;
    }
    return heap_caps_malloc(len, MALLOC_CAP_DEFAULT);
}

static esp_err_t open_encoder(void)
{
    esp_h264_enc_cfg_sw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_I420,
        .gop = LIVE_GOP,
        .fps = MEDIA_PATTERN_FPS,
        .res = { MEDIA_PATTERN_WIDTH, MEDIA_PATTERN_HEIGHT },
        .rc = { LIVE_BITRATE_BPS, LIVE_QP_MIN, LIVE_QP_MAX },
    };
    esp_h264_err_t err = esp_h264_enc_sw_new(&cfg, &s_live.enc);
    if (err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "cannot create the software encoder (%d)", err);
        s_live.enc = NULL;
        return ESP_FAIL;
    }
    err = esp_h264_enc_open(s_live.enc);
    if (err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "cannot open the software encoder (%d)", err);
        esp_h264_enc_del(s_live.enc);
        s_live.enc = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void close_encoder(void)
{
    if (s_live.enc == NULL) {
        return;
    }
    esp_h264_enc_close(s_live.enc);
    esp_h264_enc_del(s_live.enc);
    s_live.enc = NULL;
}

/* Renders and encodes the next pattern frame. Output lands in s_live.annexb
 * (Annex-B framing, SPS/PPS prepended on IDR frames). */
static esp_err_t encode_one(size_t *annexb_len, bool *idr)
{
    media_pattern_generate(s_live.yuv, yuv_len(), s_live.frame_index);

    esp_h264_enc_in_frame_t in = {
        .raw_data = { .buffer = s_live.yuv, .len = (uint32_t)yuv_len() },
        .pts = s_live.pts_ms,
    };
    esp_h264_enc_out_frame_t out = {
        .raw_data = { .buffer = s_live.annexb, .len = LIVE_AU_CAP },
    };
    esp_h264_err_t err = esp_h264_enc_process(s_live.enc, &in, &out);
    if (err != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "encode failed (%d)", err);
        return ESP_FAIL;
    }
    if (out.frame_type == ESP_H264_FRAME_TYPE_INVALID || out.length == 0) {
        ESP_LOGE(TAG, "encoder produced no frame");
        return ESP_FAIL;
    }

    s_live.frame_index++;
    s_live.pts_ms += 1000 / MEDIA_PATTERN_FPS;
    *annexb_len = out.length;
    *idr = out.frame_type == ESP_H264_FRAME_TYPE_IDR;
    return ESP_OK;
}

static bool capture_codec_record(size_t annexb_len)
{
    /* The warm-up frame is an IDR, so its stream starts with SPS and PPS. */
    const uint8_t *sps = NULL;
    const uint8_t *pps = NULL;
    size_t sps_len = 0;
    size_t pps_len = 0;

    media_nal_iter_t it;
    media_nal_begin(&it, s_live.annexb, annexb_len);
    while (media_nal_next(&it)) {
        if (it.type == 7 && sps == NULL) {
            sps = s_live.annexb + it.offset;
            sps_len = it.nal_len;
        } else if (it.type == 8 && pps == NULL) {
            pps = s_live.annexb + it.offset;
            pps_len = it.nal_len;
        }
    }
    if (sps == NULL || pps == NULL) {
        ESP_LOGE(TAG, "first encode produced no SPS/PPS");
        return false;
    }
    s_live.avcc_len = media_avcc_record(sps, sps_len, pps, pps_len, s_live.avcc, sizeof(s_live.avcc));
    if (s_live.avcc_len == 0) {
        ESP_LOGE(TAG, "codec record does not fit %u bytes", (unsigned)sizeof(s_live.avcc));
        return false;
    }
    return true;
}

static const uint8_t *live_codec_record(void *user, size_t *len)
{
    (void)user;

    /* A new streaming epoch: rebuild the encoder so the receiver's first frame
     * is an IDR with fresh parameter sets, however long the device idled or
     * whatever the previous session sent. */
    close_encoder();
    s_live.have_pending = false;
    if (open_encoder() != ESP_OK) {
        return NULL;
    }

    bool idr = false;
    size_t annexb_len = 0;
    if (encode_one(&annexb_len, &idr) != ESP_OK || !idr) {
        ESP_LOGE(TAG, "the first encode is not an IDR; encoder state is broken");
        close_encoder();
        return NULL;
    }
    if (!capture_codec_record(annexb_len)) {
        close_encoder();
        return NULL;
    }
    /* Park the epoch's first frame in the delivery buffer; the session's first
     * next_frame() call sends it. */
    if (!media_avcc_wrap(s_live.annexb, annexb_len, s_live.au, LIVE_AU_CAP, &s_live.pending_len,
                         &idr)) {
        ESP_LOGE(TAG, "access unit does not fit %u bytes", (unsigned)LIVE_AU_CAP);
        close_encoder();
        return NULL;
    }

    s_live.have_pending = true;
    *len = s_live.avcc_len;
    return s_live.avcc;
}

static esp_err_t live_next_frame(void *user, const uint8_t **au, size_t *len, bool *idr)
{
    (void)user;

    if (s_live.have_pending) {
        s_live.have_pending = false;
        *au = s_live.au;
        *len = s_live.pending_len;
        *idr = true;
        return ESP_OK;
    }

    size_t annexb_len = 0;
    if (encode_one(&annexb_len, idr) != ESP_OK) {
        return ESP_FAIL;
    }
    if (!media_avcc_wrap(s_live.annexb, annexb_len, s_live.au, LIVE_AU_CAP, len, idr)) {
        ESP_LOGE(TAG, "access unit does not fit %u bytes", (unsigned)LIVE_AU_CAP);
        return ESP_FAIL;
    }
    *au = s_live.au;
    return ESP_OK;
}

static const airplay_live_source_t s_source = {
    .width = MEDIA_PATTERN_WIDTH,
    .height = MEDIA_PATTERN_HEIGHT,
    .interval_ms = 1000 / MEDIA_PATTERN_FPS,
    .max_au_len = LIVE_AU_CAP,
    .codec_record = live_codec_record,
    .next_frame = live_next_frame,
    .user = NULL,
};

bool media_live_init(void)
{
    if (s_live.ready) {
        return true;
    }

    s_live.yuv = big_alloc(yuv_len());
    s_live.annexb = big_alloc(LIVE_AU_CAP);
    s_live.au = big_alloc(LIVE_AU_CAP);
    if (s_live.yuv == NULL || s_live.annexb == NULL || s_live.au == NULL) {
        ESP_LOGE(TAG, "no memory for the encoder buffers (free heap %u, largest block %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        free(s_live.yuv);
        free(s_live.annexb);
        free(s_live.au);
        s_live.yuv = NULL;
        s_live.annexb = NULL;
        s_live.au = NULL;
        return false;
    }

    if (open_encoder() != ESP_OK) {
        free(s_live.yuv);
        free(s_live.annexb);
        free(s_live.au);
        s_live.yuv = NULL;
        s_live.annexb = NULL;
        s_live.au = NULL;
        return false;
    }

    s_live.ready = true;
    ESP_LOGI(TAG, "live video: %dx%d @ %d fps, GOP %d, encoder ready",
             MEDIA_PATTERN_WIDTH, MEDIA_PATTERN_HEIGHT, MEDIA_PATTERN_FPS, LIVE_GOP);
    return true;
}

bool media_live_ready(void)
{
    return s_live.ready;
}

const airplay_live_source_t *media_live_source(void)
{
    return s_live.ready ? &s_source : NULL;
}
