#include "airplay_screen_camera.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "airplay/airplay_service.h"
#include "board.h"
#include "jpg/image_to_jpeg.h"
#include "system_info.h"

#define TAG "AirplayCam"

AirplayScreenCamera::AirplayScreenCamera(uint16_t width, uint16_t height)
    : width_(width), height_(height) {}

AirplayScreenCamera::~AirplayScreenCamera() {
    heap_caps_free(frame_);
    frame_ = nullptr;
}

void AirplayScreenCamera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool AirplayScreenCamera::Capture() {
    auto& service = AirplayService::GetInstance();
    const size_t bytes = service.ScreenBytes();
    if (bytes == 0) {
        ESP_LOGE(TAG, "no virtual display to look at");
        return false;
    }
    if (bytes != frame_len_) {
        heap_caps_free(frame_);
        frame_ = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        frame_len_ = frame_ != nullptr ? bytes : 0;
        if (frame_ == nullptr) {
            ESP_LOGE(TAG, "failed to allocate %u bytes for the snapshot", (unsigned)bytes);
            return false;
        }
    }
    return service.CopyScreen(frame_, frame_len_);
}

bool AirplayScreenCamera::SetHMirror(bool) { return false; }
bool AirplayScreenCamera::SetVFlip(bool) { return false; }

/* Same upload shape as the hardware camera path (EspVideo::Explain): a
 * chunked multipart/form-data POST with the question and the JPEG, answered
 * by the server's vision model. The device itself never interprets pixels. */
std::expected<std::string, std::string> AirplayScreenCamera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        return std::unexpected("Image explain URL or token is not set");
    }
    if (frame_ == nullptr || frame_len_ == 0) {
        return std::unexpected("No frame captured");
    }

    uint8_t* jpeg = nullptr;
    size_t jpeg_len = 0;
    if (!image_to_jpeg(frame_, frame_len_, width_, height_, V4L2_PIX_FMT_RGB565, 80, &jpeg,
                       &jpeg_len) ||
        jpeg == nullptr || jpeg_len == 0) {
        return std::unexpected("Failed to encode image to JPEG");
    }

    auto result = ExplainJpeg(jpeg, jpeg_len, question);
    heap_caps_free(jpeg);
    if (result) {
        ESP_LOGI(TAG, "Explain: snapshot %u bytes -> jpeg %u bytes, question=%s",
                 (unsigned)frame_len_, (unsigned)jpeg_len, question.c_str());
    }
    return result;
}

std::expected<std::string, std::string> AirplayScreenCamera::ExplainJpeg(
        const uint8_t* jpeg, size_t jpeg_len, const std::string& question) {
    if (explain_url_.empty()) {
        return std::unexpected("Image explain URL or token is not set");
    }
    if (jpeg == nullptr || jpeg_len == 0) {
        return std::unexpected("No image data");
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    /* Ten seconds instead of the 30-second default: an unresponsive vision
     * node then fails fast enough for the assistant to retry a different
     * one, instead of stalling the conversation for half a minute. */
    http->SetTimeout(10000);
    const std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");

    auto open = http->Open("POST", explain_url_);
    if (!open) {
        ESP_LOGE(TAG, "Failed to connect to explain URL: %s", open.error().ToString().c_str());
        return std::unexpected("Failed to connect to explain URL");
    }

    /* All body parts go out through one writer; a failure anywhere aborts. */
    auto write = [&http](const char* data, size_t size) -> std::string {
        if (auto written = http->Write(data, size); !written) {
            return written.error().ToString();
        }
        return "";
    };

    std::string err;
    {
        std::string part;
        part += "--" + boundary + "\r\n";
        part += "Content-Disposition: form-data; name=\"question\"\r\n";
        part += "\r\n";
        part += question + "\r\n";
        part += "--" + boundary + "\r\n";
        part += "Content-Disposition: form-data; name=\"file\"; filename=\"screen.jpg\"\r\n";
        part += "Content-Type: image/jpeg\r\n";
        part += "\r\n";
        err = write(part.data(), part.size());
    }
    if (err.empty()) {
        err = write(reinterpret_cast<const char*>(jpeg), jpeg_len);
    }
    if (err.empty()) {
        std::string footer;
        footer += "\r\n--" + boundary + "--\r\n";
        err = write(footer.data(), footer.size());
    }
    if (err.empty()) {
        err = write("", 0);
    }
    const size_t jpeg_sent = jpeg_len;
    if (!err.empty()) {
        ESP_LOGE(TAG, "Failed to upload snapshot: %s", err.c_str());
        http->Close();
        return std::unexpected("Failed to upload snapshot: " + err);
    }

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        http->Close();
        return std::unexpected("Failed to read the upload status");
    }
    if (*status_code != 200) {
        ESP_LOGE(TAG, "Explain upload rejected, status %d", *status_code);
        http->Close();
        return std::unexpected("Image server returned status " + std::to_string(*status_code));
    }

    std::string result = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "Explain: snapshot %u bytes -> jpeg %u bytes, question=%s", (unsigned)frame_len_,
             (unsigned)jpeg_sent, question.c_str());
    return result;
}
