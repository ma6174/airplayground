#include "airplay_http.h"

#include <cstring>
#include <string>

#include <esp_http_server.h>
#include <esp_log.h>

#include "airplay_service.h"

#define TAG "AirplayHttp"

namespace {

constexpr int kMaxQuery = 96;

bool QueryValue(httpd_req_t* req, const char* key, char* out, size_t out_len) {
    char query[kMaxQuery];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char value[kMaxQuery];
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return false;
    }
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
    };
    for (const auto& uri : uris) {
        httpd_register_uri_handler(server, &uri);
    }
    ESP_LOGI(TAG, "bench API on http://<device>:8080/airplay/{status,start,stop,pin}");
}
