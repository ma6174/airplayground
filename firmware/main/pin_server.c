#include "pin_server.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "pin_server";

static httpd_handle_t s_server;
static QueueHandle_t s_codes;

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

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot start the pairing page on port %d: %s", PIN_SERVER_PORT,
                 esp_err_to_name(err));
        return err;
    }

    httpd_uri_t form = {.uri = "/", .method = HTTP_GET, .handler = handle_form};
    httpd_uri_t pin = {.uri = "/pin", .method = HTTP_POST, .handler = handle_pin};
    httpd_register_uri_handler(s_server, &form);
    httpd_register_uri_handler(s_server, &pin);
    ESP_LOGI(TAG, "pairing page on http://<device>:%d/", PIN_SERVER_PORT);
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
