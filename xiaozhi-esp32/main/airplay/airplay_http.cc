#include "airplay_http.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <mbedtls/base64.h>

#include <cJSON.h>

#include "airplay_service.h"
#include "display/virtual_display.h"
#include "webcast_client.h"

extern "C" {
#include "media_net.h"
}

#define TAG "AirplayHttp"

namespace {

constexpr int kMaxQuery = 96;
/* The longest query value any endpoint takes: an image URL. */
constexpr size_t kMaxValue = 640;
/* One screen of text; the POST body carries it raw so the model can send any
 * UTF-8 without percent-encoding games. */
constexpr size_t kMaxText = 512;
/* /vision/explain JSON body cap: base64 of a 1600px JPEG plus the question.
 * One megabyte covers it with room to spare and stays PSRAM-friendly. */
constexpr size_t kMaxVisionBody = 1024 * 1024;

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

/* `query_len` covers endpoints with long values (image URLs): the default
 * kMaxQuery buffer cannot hold them. */
bool QueryValue(httpd_req_t* req, const char* key, char* out, size_t out_len,
                size_t query_len = kMaxQuery) {
    std::vector<char> query(query_len);
    if (httpd_req_get_url_query_str(req, query.data(), query_len) != ESP_OK) {
        return false;
    }
    char value[kMaxValue];
    /* A caller that asks for a buffer as large as this one would lose the
     * terminating NUL: the value is copied out with room to spare. */
    if (out_len > sizeof(value)) {
        return false;
    }
    if (httpd_query_key_value(query.data(), key, value, sizeof(value)) != ESP_OK) {
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

/* "host:port" makes the device dial a streaming server instead of waiting for
 * one. A host that cannot reach the device - a laptop behind NAT, a machine on
 * another network - still gets its screen on the TV this way, and the target is
 * kept in NVS so a reboot resumes without asking again. An empty host clears it. */
esp_err_t PullHandler(httpd_req_t* req) {
    char host[kMaxQuery] = {0};
    QueryValue(req, "host", host, sizeof(host));
    if (host[0] == '\0') {
        media_net_pull_stop();
        return Reply(req, "{\"ok\":true,\"pull\":\"\"}");
    }
    std::string target(host);
    uint16_t port = 8090;
    const size_t colon = target.rfind(':');
    if (colon != std::string::npos) {
        port = static_cast<uint16_t>(strtoul(target.c_str() + colon + 1, nullptr, 10));
        target.erase(colon);
    }
    if (!media_net_pull(target.c_str(), port)) {
        return Reply(req, "{\"ok\":false,\"error\":\"bad pull target\"}");
    }
    /* The capture stream is the newer request: stop the webcast pull so it
     * does not keep a second screen stream running out of sight. */
    WebcastClient::GetInstance().Stop();
    char body[128];
    snprintf(body, sizeof(body), "{\"ok\":true,\"pull\":\"%s:%u\"}", target.c_str(),
             static_cast<unsigned>(port));
    return Reply(req, body);
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

/* Sets the webcaster address (host:port) the device pulls pages from; the
 * alternative to the retired MCP tool, since the address rarely changes. */
esp_err_t WebcastHostHandler(httpd_req_t* req) {
    char host[64] = {0};
    if (!QueryValue(req, "host", host, sizeof(host)) || host[0] == '\0') {
        return Reply(req, "{\"ok\":false,\"error\":\"missing host\"}");
    }
    WebcastClient::GetInstance().SetHost(host);
    return Reply(req, "{\"ok\":true}");
}

/* Bench entry points for the page stream: mirror of what the MCP tools do,
 * so the chain is testable without speech. Opening a page does not start
 * mirroring by itself - POST /airplay/start separately, exactly like the
 * voice path's ordering. */
esp_err_t WebcastOpenHandler(httpd_req_t* req) {
    char url[256] = {0};
    if (!QueryValue(req, "url", url, sizeof(url)) || url[0] == '\0') {
        return Reply(req, "{\"ok\":false,\"error\":\"missing url\"}");
    }
    const bool ok = WebcastClient::GetInstance().Open(url);
    if (ok) {
        /* One source on screen at a time. A capture client that keeps pushing
         * would win the per-session source pick, so let it go: whoever was
         * asked for most recently is what the user wants to see. */
        media_net_pull_stop();
        media_net_drop_client();
    }
    return Reply(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

esp_err_t WebcastStopHandler(httpd_req_t* req) {
    WebcastClient::GetInstance().Stop();
    return Reply(req, "{\"ok\":true}");
}

/* POST /airplay/image[?url=...]: the body is a JPEG picture (Content-Length
 * required), or `url` points at one and the device downloads it. The picture is
 * decoded and put on the screen, so a running session mirrors it to the TV. */
esp_err_t ImageHandler(httpd_req_t* req) {
    auto& service = AirplayService::GetInstance();
    char url[512] = {0};
    if (QueryValue(req, "url", url, sizeof(url), 640)) {
        std::string error;
        if (!service.ShowImageFromUrl(url, &error)) {
            return Reply(req, (std::string("{\"ok\":false,\"error\":\"") + error + "\"}").c_str());
        }
        return Reply(req, "{\"ok\":true}");
    }

    char length_hdr[16] = {0};
    size_t total = 0;
    if (httpd_req_get_hdr_value_str(req, "Content-Length", length_hdr, sizeof(length_hdr)) == ESP_OK) {
        total = strtoul(length_hdr, nullptr, 10);
    }
    if (total == 0 || total > 4u * 1024 * 1024) {
        return Reply(req, "{\"ok\":false,\"error\":\"send the JPEG as the body with a Content-Length of up to 4 MB, or use ?url=\"}");
    }
    auto* body = static_cast<uint8_t*>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM));
    if (body == nullptr) {
        return Reply(req, "{\"ok\":false,\"error\":\"no memory\"}");
    }
    size_t got = 0;
    while (got < total) {
        const int want = static_cast<int>(std::min(total - got, static_cast<size_t>(8192)));
        const int read = httpd_req_recv(req, reinterpret_cast<char*>(body) + got, want);
        if (read <= 0) {
            break;
        }
        got += static_cast<size_t>(read);
    }
    if (got != total) {
        heap_caps_free(body);
        return Reply(req, "{\"ok\":false,\"error\":\"body shorter than Content-Length\"}");
    }
    std::string error;
    const bool ok = service.ShowImageFromJpeg(body, total, &error);
    heap_caps_free(body);
    if (!ok) {
        return Reply(req, (std::string("{\"ok\":false,\"error\":\"") + error + "\"}").c_str());
    }
    return Reply(req, "{\"ok\":true}");
}

/* POST /airplay/photo?url=...[&host=...]: forwards the picture to a receiver
 * with the AirPlay photo protocol (PUT /photo), no mirroring session involved. */
esp_err_t PhotoHandler(httpd_req_t* req) {
    char url[512] = {0};
    char host[96] = {0};
    if (!QueryValue(req, "url", url, sizeof(url), 640)) {
        return Reply(req, "{\"ok\":false,\"error\":\"missing url\"}");
    }
    QueryValue(req, "host", host, sizeof(host), 640);
    return Reply(req, AirplayService::GetInstance().SendPhoto(url, host).c_str());
}

/* Body is the text itself; an empty body hands the screen back to the chat UI.
 * Query style options: color=RRGGBB, background=RRGGBB, size=auto|large|medium|small. */
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

    DisplayTextStyle style;
    char value[kMaxQuery];
    if (QueryValue(req, "color", value, sizeof(value))) {
        if (!ParseHexColor(value, &style.color)) {
            return Reply(req, "{\"ok\":false,\"error\":\"color must be RRGGBB\"}");
        }
        style.has_color = true;
    }
    if (QueryValue(req, "background", value, sizeof(value))) {
        if (!ParseHexColor(value, &style.background)) {
            return Reply(req, "{\"ok\":false,\"error\":\"background must be RRGGBB\"}");
        }
        style.has_background = true;
    }
    if (QueryValue(req, "size", value, sizeof(value))) {
        if (!ParseTextSize(value, &style.size)) {
            return Reply(req, "{\"ok\":false,\"error\":\"size must be auto, large, medium or small\"}");
        }
    }

    if (!AirplayService::GetInstance().ShowText(body, style)) {
        return Reply(req, "{\"ok\":false,\"error\":\"no display\"}");
    }
    return Reply(req, total == 0 ? "{\"ok\":true,\"content\":\"chat\"}"
                                 : "{\"ok\":true,\"content\":\"text\"}");
}

}  // namespace

static httpd_handle_t s_server = nullptr;
static VisionExplainFn s_vision_explain;

void AirplayHttpSetVisionExplain(VisionExplainFn fn) { s_vision_explain = std::move(fn); }

/* Body: {"question": "...", "image": "<base64 JPEG>"}. Forwards the image to
 * the cloud vision pipeline and returns its answer text. The upload inside
 * takes seconds, so callers should keep generous HTTP timeouts. */
esp_err_t VisionExplainHandler(httpd_req_t* req) {
    if (!s_vision_explain) {
        return Reply(req, "{\"ok\":false,\"error\":\"vision explain not wired on this board\"}");
    }
    const int content_len = req->content_len;
    if (content_len <= 0 || static_cast<size_t>(content_len) > kMaxVisionBody) {
        return Reply(req, "{\"ok\":false,\"error\":\"body missing or too large\"}");
    }
    auto* body = static_cast<char*>(heap_caps_malloc(content_len + 1, MALLOC_CAP_SPIRAM));
    if (body == nullptr) {
        return Reply(req, "{\"ok\":false,\"error\":\"no memory\"}");
    }
    size_t total = 0;
    while (total < static_cast<size_t>(content_len)) {
        const int want = content_len - static_cast<int>(total);
        const int got = httpd_req_recv(req, body + total, want);
        if (got <= 0) {
            break;
        }
        total += static_cast<size_t>(got);
    }
    body[total] = '\0';

    std::string reply;
    cJSON* json = cJSON_Parse(body);
    heap_caps_free(body);
    const cJSON* question = json ? cJSON_GetObjectItem(json, "question") : nullptr;
    const cJSON* image = json ? cJSON_GetObjectItem(json, "image") : nullptr;
    if (!cJSON_IsString(question) || !cJSON_IsString(image)) {
        reply = "{\"ok\":false,\"error\":\"question and image (base64 JPEG) required\"}";
    } else {
        const size_t b64_len = strlen(image->valuestring);
        const size_t jpeg_cap = b64_len / 4 * 3 + 3;
        auto* jpeg = static_cast<uint8_t*>(heap_caps_malloc(jpeg_cap, MALLOC_CAP_SPIRAM));
        size_t jpeg_len = 0;
        if (jpeg == nullptr ||
            mbedtls_base64_decode(jpeg, jpeg_cap, &jpeg_len,
                                  reinterpret_cast<const unsigned char*>(image->valuestring),
                                  b64_len) != 0 ||
            jpeg_len == 0) {
            reply = "{\"ok\":false,\"error\":\"invalid base64 image\"}";
        } else {
            auto result = s_vision_explain(jpeg, jpeg_len, question->valuestring);
            if (result) {
                reply = std::move(*result);
            } else {
                std::string escaped;
                for (char c : result.error()) {
                    if (c == '"' || c == '\\') escaped += '\\';
                    escaped += c;
                }
                reply = "{\"ok\":false,\"error\":\"" + escaped + "\"}";
            }
        }
        heap_caps_free(jpeg);
    }
    cJSON_Delete(json);
    return Reply(req, reply.c_str());
}

void AirplayHttpStart() {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.lru_purge_enable = true;
    /* Eleven endpoints now; the default cap of 8 silently dropped the rest. */
    config.max_uri_handlers = 16;
    /* The screenshot endpoint runs the software H.264 encoder in the handler. */
    config.stack_size = 32768;
    /* Built-ins plus board-registered endpoints; over the default 8 new URIs
     * would silently 404. */
    config.max_uri_handlers = 16;
    /* The bench API is polled by one client at a time, so holding seven sockets
     * open is pure loss: a mirroring session plus a pull connection can need
     * every one of them, and when the pool runs out httpd stops accepting and
     * /airplay/status goes silent. */
    config.max_open_sockets = 4;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "cannot start the debug HTTP server");
        return;
    }
    s_server = server;

    const httpd_uri_t uris[] = {
        {.uri = "/airplay/status", .method = HTTP_GET, .handler = StatusHandler, .user_ctx = nullptr},
        {.uri = "/airplay/start", .method = HTTP_GET, .handler = StartHandler, .user_ctx = nullptr},
        {.uri = "/airplay/stop", .method = HTTP_GET, .handler = StopHandler, .user_ctx = nullptr},
        {.uri = "/airplay/pull", .method = HTTP_GET, .handler = PullHandler, .user_ctx = nullptr},
        {.uri = "/airplay/pin", .method = HTTP_GET, .handler = PinHandler, .user_ctx = nullptr},
        {.uri = "/airplay/screen.raw", .method = HTTP_GET, .handler = ScreenHandler, .user_ctx = nullptr},
        {.uri = "/airplay/frame.h264", .method = HTTP_GET, .handler = FrameHandler, .user_ctx = nullptr},
        {.uri = "/airplay/discover", .method = HTTP_GET, .handler = DiscoverHandler, .user_ctx = nullptr},
        {.uri = "/airplay/webcast_host", .method = HTTP_GET, .handler = WebcastHostHandler, .user_ctx = nullptr},
        {.uri = "/airplay/webcast_open", .method = HTTP_GET, .handler = WebcastOpenHandler, .user_ctx = nullptr},
        {.uri = "/airplay/webcast_stop", .method = HTTP_GET, .handler = WebcastStopHandler, .user_ctx = nullptr},
        {.uri = "/airplay/text", .method = HTTP_POST, .handler = TextHandler, .user_ctx = nullptr},
        {.uri = "/airplay/image", .method = HTTP_POST, .handler = ImageHandler, .user_ctx = nullptr},
        {.uri = "/airplay/photo", .method = HTTP_POST, .handler = PhotoHandler, .user_ctx = nullptr},
        {.uri = "/vision/explain", .method = HTTP_POST, .handler = VisionExplainHandler, .user_ctx = nullptr},
    };
    for (const auto& uri : uris) {
        httpd_register_uri_handler(server, &uri);
    }
    ESP_LOGI(TAG,
             "bench API on http://<device>:8080/airplay/"
             "{status,start,stop,pull,pin,screen.raw,frame.h264,discover,text,image,photo} + "
             "/vision/explain");
}
