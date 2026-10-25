#include "webcast_client.h"

#include <cctype>
#include <cinttypes>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include "display/virtual_display.h"
#include "settings.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "driver/jpeg_decode.h"
#endif

#define TAG "Webcast"

namespace {

/* No default webcaster: a frame source is deployed per site and configured
 * with the bench endpoint /airplay/webcast_host?host=ip:port. Until then the
 * webcast tools report that no source is set. */
constexpr const char* kDefaultHost = "";
/* Keepalives arrive every 2 s, so six seconds without any byte is dead. */
constexpr int64_t kStalledMs = 6000;
constexpr size_t kMaxJpeg = 256 * 1024;
constexpr int kHttpTimeoutMs = 8000;
constexpr int kOpenTimeoutMs = 25000;
constexpr int kStreamTimeoutMs = 5000;

int64_t NowMs() {
    return esp_timer_get_time() / 1000;
}

/* A hand-rolled HTTP/1.1 client over a lwIP socket: the webcaster protocol is
 * ours on both ends, and pulling esp_http_client (plus tcp_transport and
 * esp-tls with it) cost tens of flash kilobytes the OTA slot does not have. */

int Dial(const std::string& host, const std::string& port, int timeout_ms) {
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &list) != 0 || list == nullptr) {
        return -1;
    }
    int fd = socket(list->ai_family, SOCK_STREAM, 0);
    if (fd < 0) {
        freeaddrinfo(list);
        return -1;
    }
    /* Non-blocking connect so a dead server cannot stall the caller for the
     * whole TCP timeout. */
    int nonblock = 1;
    ioctl(fd, FIONBIO, &nonblock);
    bool up = connect(fd, list->ai_addr, list->ai_addrlen) == 0;
    if (!up && errno == EINPROGRESS) {
        fd_set writers;
        FD_ZERO(&writers);
        FD_SET(fd, &writers);
        timeval deadline = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
        up = select(fd + 1, nullptr, &writers, nullptr, &deadline) == 1;
        if (up) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            up = err == 0;
        }
    }
    nonblock = 0;
    ioctl(fd, FIONBIO, &nonblock);
    freeaddrinfo(list);
    if (!up) {
        close(fd);
        return -1;
    }
    const timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

/* Opens the connection, sends the request and consumes the response head.
 * Returns the fd positioned at the body start, or -1. */
int HttpOpen(const std::string& hostport, const char* method, const std::string& path,
             const std::string& body, int timeout_ms) {
    const auto colon = hostport.rfind(':');
    const std::string host =
        colon == std::string::npos ? hostport : hostport.substr(0, colon);
    const std::string port = colon == std::string::npos ? "80" : hostport.substr(colon + 1);
    const int fd = Dial(host, port, timeout_ms);
    if (fd < 0) {
        return -1;
    }
    std::string request = std::string(method) + " " + path + " HTTP/1.1\r\nHost: " + hostport +
                          "\r\nConnection: close\r\n";
    if (!body.empty()) {
        request += "Content-Type: text/plain; charset=utf-8\r\nContent-Length: " +
                   std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n";
    request += body;

    size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t n = send(fd, request.data() + sent, request.size() - sent, 0);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        sent += static_cast<size_t>(n);
    }

    std::string head;
    char c;
    while (head.size() < 4096) {
        if (recv(fd, &c, 1, 0) <= 0) {
            close(fd);
            return -1;
        }
        head += c;
        if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) {
            break;
        }
    }
    /* "HTTP/1.x 2xx ..." - anything else (404, 500) means no body worth reading. */
    if (head.compare(0, 5, "HTTP/") != 0 || !std::isdigit(static_cast<unsigned char>(head[9])) ||
        head[9] != '2') {
        close(fd);
        return -1;
    }
    return fd;
}

/* recv may return less than asked; a frame is only complete when every byte
 * is in. False means the connection is gone. */
bool RecvFull(int fd, uint8_t* dst, size_t len) {
    size_t got = 0;
    while (got < len) {
        const ssize_t n = recv(fd, reinterpret_cast<char*>(dst) + got,
                               static_cast<size_t>(len - got), 0);
        if (n <= 0) {
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

/* Short control responses: read until the server closes (Connection: close). */
std::string RecvAll(int fd) {
    std::string out;
    char buffer[512];
    ssize_t n;
    while ((n = recv(fd, buffer, sizeof(buffer), 0)) > 0) {
        out.append(buffer, static_cast<size_t>(n));
        if (out.size() > 256 * 1024) {
            break;
        }
    }
    return out;
}

}  // namespace

WebcastClient& WebcastClient::GetInstance() {
    static WebcastClient instance;
    return instance;
}

void WebcastClient::AttachDisplay(VirtualDisplay* display) {
    display_ = display;
}

std::string WebcastClient::Host() const {
    if (host_.empty()) {
        Settings settings("webcast", false);
        host_ = settings.GetString("host", kDefaultHost);
    }
    return host_;
}

void WebcastClient::SetHost(const std::string& host) {
    host_ = host;
    Settings settings("webcast", true);
    settings.SetString("host", host);
    /* The pull task dials Host() per reconnect, so a running stream moves to
     * the new address on its next reconnect. */
}

bool WebcastClient::stalled() const {
    return running_.load() && NowMs() - last_rx_ms_.load() > kStalledMs;
}

std::string WebcastClient::Request(const char* method, const std::string& path,
                                   const std::string& body, int timeout_ms) {
    const int fd = HttpOpen(Host(), method, path, body, timeout_ms);
    if (fd < 0) {
        return "";
    }
    std::string out = RecvAll(fd);
    close(fd);
    return out;
}

bool WebcastClient::Open(const std::string& url) {
    /* The body carries the URL raw: query encoding would mangle CJK. */
    const std::string response = Request("POST", "/open", url, kOpenTimeoutMs);
    if (response.find("\"ok\": true") == std::string::npos) {
        return false;
    }
    if (!running_.load()) {
        stop_requested_ = false;
        running_ = true;
        if (xTaskCreate(StreamTask, "webcast", 8192, this, 4, &task_) != pdPASS) {
            running_ = false;
            return false;
        }
    }
    return true;
}

void WebcastClient::Stop() {
    if (!running_.load()) {
        return;
    }
    stop_requested_ = true;
    Request("POST", "/close", "", kHttpTimeoutMs);
    for (int i = 0; i < 80 && running_.load(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    stop_requested_ = false;
    if (display_ != nullptr) {
        display_->ShowWebcast(false);
    }
}

void WebcastClient::StreamTask(void* arg) {
    auto* self = static_cast<WebcastClient*>(arg);
    auto* jpeg = static_cast<uint8_t*>(heap_caps_malloc(kMaxJpeg, MALLOC_CAP_SPIRAM));

    while (!self->stop_requested_.load()) {
        const int fd = HttpOpen(self->Host(), "GET", "/stream", "", kStreamTimeoutMs);
        if (fd < 0) {
            ESP_LOGW(TAG, "stream connect failed; retrying");
            for (int i = 0; i < 30 && !self->stop_requested_.load(); i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        ESP_LOGI(TAG, "stream connected");
        self->last_rx_ms_ = NowMs();
        uint8_t header[5];
        while (!self->stop_requested_.load()) {
            if (!RecvFull(fd, header, sizeof(header))) {
                ESP_LOGW(TAG, "stream read failed");
                break;
            }
            self->last_rx_ms_ = NowMs();
            const size_t length = (static_cast<size_t>(header[0]) << 24) |
                                  (static_cast<size_t>(header[1]) << 16) |
                                  (static_cast<size_t>(header[2]) << 8) |
                                  static_cast<size_t>(header[3]);
            const uint8_t flags = header[4];
            if (flags != 0) {
                continue;  // keepalive: the connection lives, nothing to draw
            }
            if (length == 0 || length > kMaxJpeg) {
                break;  // protocol violation; resync via reconnect
            }
            if (!RecvFull(fd, jpeg, length)) {
                break;
            }
            self->last_rx_ms_ = NowMs();
            self->DecodeFrame(jpeg, length);
        }

        close(fd);
        if (!self->stop_requested_.load()) {
            ESP_LOGW(TAG, "stream ended; reconnecting");
            for (int i = 0; i < 30 && !self->stop_requested_.load(); i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }

    heap_caps_free(jpeg);
    self->running_ = false;
    self->task_ = nullptr;
    vTaskDelete(nullptr);
}

#if CONFIG_IDF_TARGET_ESP32P4

bool WebcastClient::EnsureDecoder() {
    if (decoder_ != nullptr) {
        return true;
    }
    jpeg_decode_engine_cfg_t engine_cfg = {};
    engine_cfg.timeout_ms = 1000;
    if (jpeg_new_decoder_engine(&engine_cfg, &decoder_) != ESP_OK) {
        ESP_LOGE(TAG, "cannot acquire the JPEG decoder engine");
        decoder_ = nullptr;
        return false;
    }
    jpeg_decode_memory_alloc_cfg_t mem_cfg = {};
    mem_cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    /* The engine writes the MCU-aligned picture (4:2:0 pads the height to a
     * multiple of 16), not the visible one; a 270-line frame needs 272. */
    const size_t frame_bytes =
        static_cast<size_t>(display_->width()) * (display_->height() + 16) * 2;
    decode_out_ = static_cast<uint8_t*>(
        jpeg_alloc_decoder_mem(frame_bytes, &mem_cfg, &decode_out_cap_));
    if (decode_out_ == nullptr) {
        ESP_LOGE(TAG, "no memory for the JPEG decode output");
        jpeg_del_decoder_engine(decoder_);
        decoder_ = nullptr;
        return false;
    }
    mem_cfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
    decode_in_ = static_cast<uint8_t*>(
        jpeg_alloc_decoder_mem(kMaxJpeg, &mem_cfg, &decode_in_cap_));
    if (decode_in_ == nullptr) {
        ESP_LOGE(TAG, "no memory for the JPEG decode input");
        heap_caps_free(decode_out_);
        decode_out_ = nullptr;
        jpeg_del_decoder_engine(decoder_);
        decoder_ = nullptr;
        return false;
    }
    return true;
}

bool WebcastClient::DecodeFrame(const uint8_t* jpeg, size_t len) {
    if (display_ == nullptr || !EnsureDecoder()) {
        ESP_LOGE(TAG, "decode dropped: display=%p decoder=%p", display_, decoder_);
        return false;
    }
    jpeg_decode_picture_info_t info = {};
    if (jpeg_decoder_get_info(jpeg, len, &info) != ESP_OK) {
        ESP_LOGE(TAG, "get_info failed for %u bytes (head %02x %02x %02x %02x)", (unsigned)len,
                 jpeg[0], jpeg[1], jpeg[2], jpeg[3]);
        return false;
    }
    if (info.width != static_cast<uint32_t>(display_->width()) ||
        info.height != static_cast<uint32_t>(display_->height())) {
        ESP_LOGW(TAG, "stream frame is %lux%lu, display is %dx%d; dropping",
                 (unsigned long)info.width, (unsigned long)info.height,
                 display_->width(), display_->height());
        return false;
    }
    jpeg_decode_cfg_t decode_cfg = {};
    decode_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    /* The engine's "RGB" order swaps red and blue on an RGB565 wire; BGR is
     * the packing that lands red in the high bits. */
    decode_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    decode_cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    /* The stream buffer is plain PSRAM; the engine only reads its own. */
    memcpy(decode_in_, jpeg, len);
    uint32_t out_len = 0;
    const esp_err_t err = jpeg_decoder_process(decoder_, &decode_cfg, decode_in_, len,
                                               decode_out_, decode_out_cap_, &out_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_decoder_process failed: %s", esp_err_to_name(err));
        return false;
    }
    frames_.fetch_add(1);
    return display_->PushWebcastFrame(reinterpret_cast<const uint16_t*>(decode_out_));
}

#else  // non-P4 targets have no hardware JPEG decoder yet

bool WebcastClient::DecodeFrame(const uint8_t*, size_t) {
    return false;
}

#endif

std::string WebcastClient::Snapshot() {
    std::string response = Request("GET", "/snapshot", "", kHttpTimeoutMs);
    return response.empty() ? "(webcaster unreachable at " + Host() + ")" : response;
}

bool WebcastClient::Click(int ref) {
    return Request("POST", "/click?ref=" + std::to_string(ref), "", kHttpTimeoutMs)
        .find("\"ok\": true") != std::string::npos;
}

bool WebcastClient::Type(int ref, const std::string& text) {
    return Request("POST", "/type?ref=" + std::to_string(ref), text, kHttpTimeoutMs)
        .find("\"ok\": true") != std::string::npos;
}

bool WebcastClient::Scroll(int dy) {
    return Request("POST", "/scroll?dy=" + std::to_string(dy), "", kHttpTimeoutMs)
        .find("\"ok\": true") != std::string::npos;
}

bool WebcastClient::Press(const std::string& key) {
    return Request("POST", "/press?key=" + key, "", kHttpTimeoutMs)
        .find("\"ok\": true") != std::string::npos;
}

std::string WebcastClient::StatusJson() const {
    char buffer[256];
    snprintf(buffer, sizeof(buffer),
             "{\"streaming\":%s,\"stalled\":%s,\"host\":\"%s\",\"frames\":%" PRIu32 "}",
             running_.load() ? "true" : "false", stalled() ? "true" : "false",
             Host().c_str(), frames_.load());
    return buffer;
}
