#include "pin_server.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "pin_server";

static httpd_handle_t s_server;
static QueueHandle_t s_codes;
static pin_server_session_hooks_t s_hooks;

/* The queue is also the wake-up: waiting on it is a real block, unlike polling
 * a flag that depends on both tasks being scheduled. */
#define PIN_QUEUE_DEPTH 2

static const char k_form[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>AirPlay pairing</title>"
    "<style>body{font:16px system-ui;margin:3em auto;max-width:22em;text-align:center}"
    "input{font-size:2em;width:5em;text-align:center;letter-spacing:.2em}"
    "button{font-size:1.2em;margin-top:1em;padding:.4em 1.2em}</style>"
    "<h1>Pairing code</h1>"
    "<p>Enter the 4 digits the receiver is showing.</p>"
    "<form method=post action=/pin>"
    "<input name=code inputmode=numeric pattern='[0-9]{4}' maxlength=4 autofocus>"
    "<br><button>Send</button></form>";

static esp_err_t handle_form(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html");
    return httpd_resp_send(request, k_form, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_pin(httpd_req_t *request)
{
    char body[96];
    int total = request->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "bad body");
    }

    int received = 0;
    while (received < total) {
        int got = httpd_req_recv(request, body + received, total - received);
        if (got <= 0) {
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "short body");
        }
        received += got;
    }
    body[received] = '\0';

    const char *field = strstr(body, "code=");
    if (field == NULL) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "missing code");
    }
    field += 5;

    char code[5];
    int digits = 0;
    for (const char *cursor = field; *cursor != '\0' && digits < 4; cursor++) {
        if (*cursor >= '0' && *cursor <= '9') {
            code[digits++] = *cursor;
        } else if (*cursor != '%' && *cursor != '+') {
            break;
        }
    }
    code[digits] = '\0';
    if (digits != 4) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "expected 4 digits");
    }

    pin_server_submit(code);
    ESP_LOGI(TAG, "pairing code submitted over HTTP");
    httpd_resp_set_type(request, "text/plain");
    return httpd_resp_send(request, "ok\n", HTTPD_RESP_USE_STRLEN);
}

void pin_server_submit(const char pin[5])
{
    char copy[5];

    if (s_codes == NULL || pin == NULL || strlen(pin) != 4) {
        return;
    }
    memcpy(copy, pin, sizeof(copy));
    /* Never block the producer: an outdated code is better dropped than
     * delaying the task that owns the socket. */
    if (xQueueSend(s_codes, copy, 0) == pdTRUE) {
        ESP_LOGI(TAG, "code queued");
    }
}

void pin_server_set_session_hooks(const pin_server_session_hooks_t *hooks)
{
    s_hooks = hooks == NULL ? (pin_server_session_hooks_t){0} : *hooks;
}

/* One shared handler for /stop and /status: the answer is whatever the hook
 * returns, so curl shows a plain sentence instead of a status page. */
static esp_err_t handle_session(httpd_req_t *request)
{
    const char *(*hook)(void) = strcmp(request->user_ctx, "stop") == 0
                                    ? s_hooks.stop
                                    : s_hooks.status;

    httpd_resp_set_type(request, "text/plain");
    if (hook == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "session control not wired up\n");
    }
    return httpd_resp_send(request, hook(), HTTPD_RESP_USE_STRLEN);
}

/* Query-string values arrive percent-encoded and receiver names contain
 * spaces; decode in place. */
static void url_decode(char *text)
{
    char *out = text;
    for (char *cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '%' && isxdigit((unsigned char)cursor[1]) &&
            isxdigit((unsigned char)cursor[2])) {
            char hex[3] = {cursor[1], cursor[2], '\0'};
            *out++ = (char)strtol(hex, NULL, 16);
            cursor += 2;
        } else if (*cursor == '+') {
            *out++ = ' ';
        } else {
            *out++ = *cursor;
        }
    }
    *out = '\0';
}

static esp_err_t handle_start(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/plain");
    if (s_hooks.start == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "session control not wired up\n");
    }

    /* Values arrive percent-encoded, and UTF-8 names expand 3x, so give the
     * scratch buffer room for a longest legal name before decoding. */
    char value[192];
    pin_start_request_t start = { .port = 0, .data_mode = -1, .key_mix = -1, .iv_source = -1,
                                  .interval_ms = 0, .sps = -1, .pair = -1,
                                  .stream_port = -1 };
    char name[64] = "";
    char host[48] = "";

    char query[256];
    esp_err_t err = httpd_req_get_url_query_str(request, query, sizeof(query));
    if (err == ESP_ERR_HTTPD_RESULT_TRUNC) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "query is too long\n");
    }
    if (err == ESP_OK) {
        err = httpd_query_key_value(query, "name", value, sizeof(value));
        if (err == ESP_OK) {
            url_decode(value);
            strlcpy(name, value, sizeof(name));
        } else if (err == ESP_ERR_HTTPD_RESULT_TRUNC) {
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "name is too long\n");
        }
        err = httpd_query_key_value(query, "host", value, sizeof(value));
        if (err == ESP_OK) {
            url_decode(value);
            strlcpy(host, value, sizeof(host));
        } else if (err == ESP_ERR_HTTPD_RESULT_TRUNC) {
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "host is too long\n");
        }
        if (httpd_query_key_value(query, "port", value, sizeof(value)) == ESP_OK) {
            start.port = atoi(value);
        }
        if (httpd_query_key_value(query, "mode", value, sizeof(value)) == ESP_OK) {
            start.data_mode = atoi(value);
        }
        if (httpd_query_key_value(query, "mix", value, sizeof(value)) == ESP_OK) {
            start.key_mix = atoi(value);
        }
        if (httpd_query_key_value(query, "iv", value, sizeof(value)) == ESP_OK) {
            start.iv_source = atoi(value);
        }
        if (httpd_query_key_value(query, "sport", value, sizeof(value)) == ESP_OK) {
            start.stream_port = atoi(value);
        }
        if (httpd_query_key_value(query, "pair", value, sizeof(value)) == ESP_OK) {
            start.pair = atoi(value);
        }
        if (httpd_query_key_value(query, "sps", value, sizeof(value)) == ESP_OK) {
            start.sps = atoi(value);
        }
        if (httpd_query_key_value(query, "fps_ms", value, sizeof(value)) == ESP_OK) {
            start.interval_ms = atoi(value);
        }
    }
    start.name = name;
    start.host = host;

    return httpd_resp_send(request, s_hooks.start(&start), HTTPD_RESP_USE_STRLEN);
}

/* The browse takes seconds and the list can be long; keep it off the handler
 * stack. Single client at a time is fine here (max_open_sockets is 2 and one
 * of those belongs to the pairing page). */
static char s_target_list[1024];

static esp_err_t handle_targets(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/plain");
    if (s_hooks.targets == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "discovery not wired up\n");
    }
    if (s_hooks.targets(s_target_list, sizeof(s_target_list)) == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "receiver browse failed\n");
    }
    return httpd_resp_send(request, s_target_list, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_forget(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/plain");
    if (s_hooks.forget == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "session control not wired up\n");
    }

    char name[64] = "";
    char query[160];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "name", name, sizeof(name)) == ESP_ERR_HTTPD_RESULT_TRUNC) {
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "name is too long\n");
        }
        url_decode(name);
    }
    if (name[0] == '\0') {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "expected ?name=<receiver>\n");
    }
    return httpd_resp_send(request, s_hooks.forget(name), HTTPD_RESP_USE_STRLEN);
}

 esp_err_t pin_server_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }
    s_codes = xQueueCreate(PIN_QUEUE_DEPTH, sizeof(char[5]));
    if (s_codes == NULL) {
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = PIN_SERVER_PORT;
    config.lru_purge_enable = true;
    /* Keep the page from eating the sockets the AirPlay session needs. */
    config.max_open_sockets = 2;
    /* /start and /targets run the receiver browse on this task. */
    config.stack_size = 6144;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot start the pairing page on port %d: %s", PIN_SERVER_PORT,
                 esp_err_to_name(err));
        return err;
    }

    httpd_uri_t form = {.uri = "/", .method = HTTP_GET, .handler = handle_form};
    httpd_uri_t pin = {.uri = "/pin", .method = HTTP_POST, .handler = handle_pin};
    httpd_uri_t start = {.uri = "/start", .method = HTTP_GET, .handler = handle_start};
    httpd_uri_t targets = {.uri = "/targets", .method = HTTP_GET, .handler = handle_targets};
    httpd_uri_t forget = {.uri = "/forget", .method = HTTP_GET, .handler = handle_forget};
    httpd_uri_t stop = {.uri = "/stop", .method = HTTP_GET,
                        .handler = handle_session, .user_ctx = "stop"};
    httpd_uri_t status = {.uri = "/status", .method = HTTP_GET,
                          .handler = handle_session, .user_ctx = "status"};
    httpd_register_uri_handler(s_server, &form);
    httpd_register_uri_handler(s_server, &pin);
    httpd_register_uri_handler(s_server, &start);
    httpd_register_uri_handler(s_server, &targets);
    httpd_register_uri_handler(s_server, &forget);
    httpd_register_uri_handler(s_server, &stop);
    httpd_register_uri_handler(s_server, &status);
    ESP_LOGI(TAG, "pairing page on http://<device>:%d/ (also /targets /start /stop /status)",
             PIN_SERVER_PORT);
    return ESP_OK;
}

bool pin_server_take(char pin[5])
{
    if (s_codes == NULL) {
        return false;
    }
    return xQueueReceive(s_codes, pin, 0) == pdTRUE;
}

void pin_server_flush(void)
{
    char discarded[5];

    if (s_codes == NULL) {
        return;
    }
    while (xQueueReceive(s_codes, discarded, 0) == pdTRUE) {
        ESP_LOGW(TAG, "dropped an outdated pairing code");
    }
}

bool pin_server_wait(char pin[5], uint32_t timeout_ms)
{
    if (s_codes == NULL) {
        return false;
    }
    return xQueueReceive(s_codes, pin, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
