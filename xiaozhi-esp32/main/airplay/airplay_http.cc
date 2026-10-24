#include "airplay_http.h"

#include <cctype>
#include <cstring>
#include <cstdlib>
#include <string>

#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>

#include "airplay_service.h"

#define TAG "AirplayHttp"

namespace {

constexpr int kMaxQuery = 96;
/* One screen of text; the POST body carries it raw so the model can send any
 * UTF-8 without percent-encoding games. */
constexpr size_t kMaxText = 512;

/* esp_http_server hands the query string over verbatim, so percent-escapes and
 * '+' for space are still in it. Receiver names are UTF-8 here: the projector
 * this bench targets is "我的Play 5". */
void PercentDecode(char* value) {
    char* out = value;
    for (const char* in = value; *in != '\0'; in++) {
        if (*in == '+') {
            *out++ = ' ';
        } else if (*in == '%' && isxdigit(static_cast<unsigned char>(in[1])) &&
                   isxdigit(static_cast<unsigned char>(in[2]))) {
            const char hex[3] = {in[1], in[2], '\0'};
            *out++ = static_cast<char>(strtol(hex, nullptr, 16));
            in += 2;
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
}

bool QueryValue(httpd_req_t* req, const char* key, char* out, size_t out_len) {
    char query[kMaxQuery];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char value[kMaxQuery];
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return false;
    }
    PercentDecode(value);
    if (out_len == 0) {
        return false;
    }
    const size_t len = strnlen(value, out_len - 1);
    memcpy(out, value, len);
    out[len] = '\0';
    return true;
}

esp_err_t Reply(httpd_req_t* req, const char* text) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, text);
}

esp_err_t StatusHandler(httpd_req_t* req) {
    auto status = AirplayService::GetInstance().StatusJson();
    return Reply(req, status.c_str());
}

esp_err_t StartHandler(httpd_req_t* req) {
    char host[kMaxQuery] = {0};
    char legacy[8] = {0};
    QueryValue(req, "host", host, sizeof(host));
    QueryValue(req, "legacy", legacy, sizeof(legacy));

    auto& service = AirplayService::GetInstance();
    const bool started = service.Start(host, legacy[0] == '1' || legacy[0] == 't');
    return Reply(req, started ? "{\"started\":true}" : "{\"started\":false}");
}

esp_err_t StopHandler(httpd_req_t* req) {
    AirplayService::GetInstance().Stop();
    return Reply(req, "{\"stopping\":true}");
}

esp_err_t PinHandler(httpd_req_t* req) {
    char code[16] = {0};
    if (!QueryValue(req, "code", code, sizeof(code))) {
        return Reply(req, "{\"ok\":false,\"error\":\"missing code\"}");
    }
    const bool accepted = AirplayService::GetInstance().EnterPin(code);
    return Reply(req, accepted ? "{\"ok\":true}" : "{\"ok\":false}");
}

esp_err_t ScreenHandler(httpd_req_t* req) {
    auto& service = AirplayService::GetInstance();
    const size_t bytes = service.ScreenBytes();
    if (bytes == 0) {
        return Reply(req, "{\"ok\":false,\"error\":\"no display\"}");
    }
    auto* buffer = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (buffer == nullptr) {
        return Reply(req, "{\"ok\":false,\"error\":\"no memory\"}");
    }
    if (!service.CopyScreen(buffer, bytes)) {
        heap_caps_free(buffer);
        return Reply(req, "{\"ok\":false,\"error\":\"copy failed\"}");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    const esp_err_t err = httpd_resp_send(req, reinterpret_cast<const char*>(buffer), bytes);
    heap_caps_free(buffer);
    return err;
}

esp_err_t FrameHandler(httpd_req_t* req) {
    auto& service = AirplayService::GetInstance();
    constexpr size_t kCap = 128 * 1024;
    auto* buffer = static_cast<uint8_t*>(heap_caps_malloc(kCap, MALLOC_CAP_SPIRAM));
    if (buffer == nullptr) {
        return Reply(req, "{\"ok\":false,\"error\":\"no memory\"}");
    }
    size_t len = 0;
    if (!service.EncodeScreenFrame(buffer, kCap, &len)) {
        heap_caps_free(buffer);
        return Reply(req, "{\"ok\":false,\"error\":\"encode failed\"}");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    const esp_err_t err = httpd_resp_send(req, reinterpret_cast<const char*>(buffer), len);
    heap_caps_free(buffer);
    return err;
}

esp_err_t DiscoverHandler(httpd_req_t* req) {
    auto list = AirplayService::GetInstance().DiscoverJson();
    return Reply(req, list.c_str());
}

/* Body is the text itself; an empty body hands the screen back to the chat UI. */
esp_err_t TextHandler(httpd_req_t* req) {
    char body[kMaxText] = {0};
    size_t total = 0;
    while (total < sizeof(body) - 1) {
        const int want = static_cast<int>((sizeof(body) - 1) - total);
        const int got = httpd_req_recv(req, body + total, want);
        if (got <= 0) {
            break;
        }
        total += static_cast<size_t>(got);
        if (static_cast<size_t>(got) < static_cast<size_t>(want)) {
            break;
        }
    }
    body[total] = '\0';
    if (!AirplayService::GetInstance().ShowText(body)) {
        return Reply(req, "{\"ok\":false,\"error\":\"no display\"}");
    }
    return Reply(req, total == 0 ? "{\"ok\":true,\"content\":\"chat\"}"
                                 : "{\"ok\":true,\"content\":\"text\"}");
}

}  // namespace

void AirplayHttpStart() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.lru_purge_enable = true;
    /* The screenshot endpoint runs the software H.264 encoder in the handler. */
    config.stack_size = 32768;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "cannot start the debug HTTP server");
        return;
    }

    const httpd_uri_t uris[] = {
        {.uri = "/airplay/status", .method = HTTP_GET, .handler = StatusHandler, .user_ctx = nullptr},
        {.uri = "/airplay/start", .method = HTTP_GET, .handler = StartHandler, .user_ctx = nullptr},
        {.uri = "/airplay/stop", .method = HTTP_GET, .handler = StopHandler, .user_ctx = nullptr},
        {.uri = "/airplay/pin", .method = HTTP_GET, .handler = PinHandler, .user_ctx = nullptr},
        {.uri = "/airplay/screen.raw", .method = HTTP_GET, .handler = ScreenHandler, .user_ctx = nullptr},
        {.uri = "/airplay/frame.h264", .method = HTTP_GET, .handler = FrameHandler, .user_ctx = nullptr},
        {.uri = "/airplay/discover", .method = HTTP_GET, .handler = DiscoverHandler, .user_ctx = nullptr},
        {.uri = "/airplay/text", .method = HTTP_POST, .handler = TextHandler, .user_ctx = nullptr},
    };
    for (const auto& uri : uris) {
        httpd_register_uri_handler(server, &uri);
    }
    ESP_LOGI(TAG,
             "bench API on http://<device>:8080/airplay/"
             "{status,start,stop,pin,screen.raw,frame.h264,discover,text}");
}
