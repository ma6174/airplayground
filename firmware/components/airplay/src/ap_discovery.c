#include "ap_discovery.h"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns.h"
#include "nvs.h"

static const char *TAG = "ap_discovery";

/* Long enough for sleepy responders, short enough not to stall an HTTP
 * handler; both queries run back to back. */
#define BROWSE_TIMEOUT_MS 1500

/* Receivers that answer queries reappear on every browse, so stale ones can
 * age out. Legacy receivers only speak when they announce, which may be once
 * per power cycle: they are kept for good and persisted in NVS instead. */
#define ENTRY_TTL_TICKS pdMS_TO_TICKS(10 * 60 * 1000)

#define NVS_NAMESPACE "discovery"
#define NVS_KEY "legacy"
#define ENTRY_BYTES (AP_DISCOVERY_NAME_LEN + 16 + 2)

typedef struct {
    airplay_target_t target;
    bool via_airplay; /* an _airplay._tcp record was seen for this receiver */
    TickType_t seen;
} entry_t;

/* Shared with the mDNS service task (browse callbacks); every access holds
 * this mutex. One entry per receiver name, so DHCP address changes and the
 * airplay/raop record pairs of one device stay merged. */
static entry_t s_entries[AP_DISCOVERY_MAX_TARGETS];
static int s_entry_count;
static SemaphoreHandle_t s_lock;

static void persist_legacy_locked(void);

/* One receiver can advertise both service types; an _airplay record carries
 * the mirroring port, so it wins over a _raop record. The address always
 * follows the newest record: devices move (DHCP), ports belong to services. */
static void record_entry(const mdns_result_t *result, const char *host, const char *name,
                         bool legacy)
{
    bool via_airplay = result->service_type != NULL &&
                       strcmp(result->service_type, "_airplay") == 0;

    for (int i = 0; i < s_entry_count; i++) {
        if (strcasecmp(s_entries[i].target.name, name) != 0) {
            continue;
        }
        entry_t *entry = &s_entries[i];
        entry->seen = xTaskGetTickCount();
        bool changed = strcmp(entry->target.host, host) != 0;
        if (changed) {
            strlcpy(entry->target.host, host, sizeof(entry->target.host));
            ESP_LOGI(TAG, "receiver \"%s\" now at %s", name, host);
        }
        if (via_airplay || !entry->via_airplay) {
            changed = changed || entry->target.port != result->port ||
                      entry->target.legacy != legacy || entry->via_airplay != via_airplay;
            entry->via_airplay = via_airplay;
            entry->target.port = result->port;
            entry->target.legacy = legacy;
            ESP_LOGI(TAG, "receiver \"%s\" at %s:%u%s (%s)", name, host,
                     (unsigned)result->port, legacy ? " [legacy]" : "",
                     via_airplay ? "airplay" : "raop");
        }
        if (changed && entry->target.legacy) {
            persist_legacy_locked();
        }
        return;
    }

    if (s_entry_count >= AP_DISCOVERY_MAX_TARGETS || name[0] == '\0') {
        return;
    }
    entry_t *entry = &s_entries[s_entry_count++];
    memset(entry, 0, sizeof(*entry));
    strlcpy(entry->target.name, name, sizeof(entry->target.name));
    strlcpy(entry->target.host, host, sizeof(entry->target.host));
    entry->target.port = result->port;
    entry->target.legacy = legacy;
    entry->via_airplay = via_airplay;
    entry->seen = xTaskGetTickCount();
    ESP_LOGI(TAG, "receiver \"%s\" at %s:%u%s (%s)", name, host, (unsigned)result->port,
             legacy ? " [legacy]" : "", via_airplay ? "airplay" : "raop");
    if (legacy) {
        persist_legacy_locked();
    }
}

static bool result_is_legacy(const mdns_result_t *result)
{
    for (size_t i = 0; i < result->txt_count; i++) {
        const char *key = result->txt[i].key;
        const char *value = result->txt[i].value;
        if (key == NULL || value == NULL) {
            continue;
        }
        if ((strcmp(key, "model") == 0 && strncmp(value, "AppleTV3,2", 10) == 0) ||
            (strcmp(key, "srcvers") == 0 && strncmp(value, "220.", 4) == 0)) {
            return true;
        }
    }
    return false;
}

/* RAOP instances are named "<MAC address>@<device name>"; use the device name. */
static void entry_name(const mdns_result_t *result, char *out, size_t out_len)
{
    const char *name = result->instance_name;
    if (name != NULL && result->service_type != NULL &&
        strcmp(result->service_type, "_raop") == 0) {
        const char *at = strchr(name, '@');
        if (at != NULL && at != name) {
            bool mac_like = true;
            for (const char *cursor = name; cursor != at; cursor++) {
                if (*cursor != ':' && !isxdigit((unsigned char)*cursor)) {
                    mac_like = false;
                    break;
                }
            }
            if (mac_like) {
                name = at + 1;
            }
        }
    }
    strlcpy(out, name != NULL ? name : "", out_len);
}

/* Runs on the mDNS service task for every announcement or answer that touches
 * the browsed service types. */
static void browse_notify(mdns_result_t *result)
{
    char host[16];
    char name[AP_DISCOVERY_NAME_LEN];

    if (result->instance_name == NULL) {
        return;
    }
    entry_name(result, name, sizeof(name));
    for (mdns_ip_addr_t *addr = result->addr; addr != NULL; addr = addr->next) {
        if (addr->addr.type != ESP_IPADDR_TYPE_V4) {
            continue;
        }
        esp_ip4addr_ntoa(&addr->addr.u_addr.ip4, host, sizeof(host));
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            record_entry(result, host, name, result_is_legacy(result));
            xSemaphoreGive(s_lock);
        }
        return;
    }
    /* Record seen but not yet resolved to an IPv4 address; the cache fires
     * again once the A record lands. */
    ESP_LOGD(TAG, "receiver record \"%s\" without an IPv4 address yet", name);
}

/* Feed one-shot query results through the same path as announcements; each
 * record takes the lock on its own. */
static void collect_query_results(mdns_result_t *results)
{
    for (mdns_result_t *result = results; result != NULL; result = result->next) {
        browse_notify(result);
    }
}

static void prune_expired(void)
{
    TickType_t now = xTaskGetTickCount();
    int kept = 0;
    for (int i = 0; i < s_entry_count; i++) {
        if (s_entries[i].target.legacy || now - s_entries[i].seen < ENTRY_TTL_TICKS) {
            s_entries[kept++] = s_entries[i];
        } else {
            ESP_LOGI(TAG, "receiver \"%s\" dropped from the list (stale)",
                     s_entries[i].target.name);
        }
    }
    s_entry_count = kept;
}

/* Legacy receivers cannot be found again on demand, so what was learned is
 * written back and restored at boot. Content is compared before writing:
 * announcement storms must not wear the flash. Lock must be held. */
static void persist_legacy_locked(void)
{
    static uint8_t last[1 + AP_DISCOVERY_MAX_TARGETS * ENTRY_BYTES];
    static size_t last_len;

    uint8_t blob[sizeof(last)];
    memset(blob, 0, sizeof(blob));
    uint8_t count = 0;
    for (int i = 0; i < s_entry_count && count < AP_DISCOVERY_MAX_TARGETS; i++) {
        if (!s_entries[i].target.legacy) {
            continue;
        }
        uint8_t *slot = blob + 1 + (size_t)count * ENTRY_BYTES;
        memcpy(slot, s_entries[i].target.name, AP_DISCOVERY_NAME_LEN);
        memcpy(slot + AP_DISCOVERY_NAME_LEN, s_entries[i].target.host, 16);
        slot[AP_DISCOVERY_NAME_LEN + 16] = (uint8_t)(s_entries[i].target.port & 0xff);
        slot[AP_DISCOVERY_NAME_LEN + 17] = (uint8_t)(s_entries[i].target.port >> 8);
        count++;
    }
    blob[0] = count;
    size_t len = 1 + (size_t)count * ENTRY_BYTES;
    if (len == last_len && memcmp(blob, last, len) == 0) {
        return;
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "cannot open NVS to save the receiver list");
        return;
    }
    esp_err_t err = nvs_set_blob(handle, NVS_KEY, blob, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot save the receiver list: %s", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }
    memcpy(last, blob, len);
    last_len = len;
    nvs_close(handle);
    ESP_LOGI(TAG, "%u legacy receiver(s) saved", count);
}

static void load_persisted(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    uint8_t blob[1 + AP_DISCOVERY_MAX_TARGETS * ENTRY_BYTES];
    size_t len = sizeof(blob);
    if (nvs_get_blob(handle, NVS_KEY, blob, &len) != ESP_OK || len < 1) {
        nvs_close(handle);
        return;
    }
    nvs_close(handle);

    uint8_t count = blob[0] > AP_DISCOVERY_MAX_TARGETS ? AP_DISCOVERY_MAX_TARGETS : blob[0];
    TickType_t now = xTaskGetTickCount();
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *slot = blob + 1 + (size_t)i * ENTRY_BYTES;
        entry_t *entry = &s_entries[s_entry_count++];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->target.name, slot, AP_DISCOVERY_NAME_LEN);
        entry->target.name[AP_DISCOVERY_NAME_LEN - 1] = '\0';
        memcpy(entry->target.host, slot + AP_DISCOVERY_NAME_LEN, 15);
        entry->target.host[15] = '\0';
        entry->target.port = (uint16_t)(slot[AP_DISCOVERY_NAME_LEN + 16] |
                                        slot[AP_DISCOVERY_NAME_LEN + 17] << 8);
        entry->target.legacy = true;
        entry->via_airplay = true;
        entry->seen = now;
        ESP_LOGI(TAG, "restored legacy receiver \"%s\" at %s:%u", entry->target.name,
                 entry->target.host, (unsigned)entry->target.port);
    }
}

/* Refreshes the list periodically so HTTP handlers never wait on a browse.
 * Runs at low priority: a browse takes ~3 s and is pure UDP work. */
static void refresh_task(void *argument)
{
    (void)argument;
    for (;;) {
        ap_discovery_browse();
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

esp_err_t ap_discovery_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        return err;
    }
    load_persisted();
    /* The browse keeps running for the lifetime of the app: it records every
     * announcement, including the boot-time burst from receivers that never
     * answer queries. */
    if (mdns_browse_new("_airplay", "_tcp", browse_notify) == NULL ||
        mdns_browse_new("_raop", "_tcp", browse_notify) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(refresh_task, "disc_refresh", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "cannot start the periodic refresh; /targets will browse on demand");
    }
    return ESP_OK;
}

int ap_discovery_browse(void)
{
    static const char *services[] = {"_airplay", "_raop"};

    int failures = 0;
    for (size_t i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
        mdns_result_t *results = NULL;
        esp_err_t err = mdns_query_ptr(services[i], "_tcp", BROWSE_TIMEOUT_MS,
                                       AP_DISCOVERY_MAX_TARGETS, &results);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mDNS query for %s._tcp failed: %s", services[i],
                     esp_err_to_name(err));
            failures++;
            continue;
        }
        collect_query_results(results);
        mdns_query_results_free(results);
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        prune_expired();
        xSemaphoreGive(s_lock);
    }
    return failures == 2 ? -1 : s_entry_count;
}

int ap_discovery_snapshot(airplay_target_t *out, int max)
{
    int count = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return 0;
    }
    for (int i = 0; i < s_entry_count && count < max; i++) {
        out[count++] = s_entries[i].target;
    }
    xSemaphoreGive(s_lock);
    return count;
}

void ap_discovery_remember(const char *name, const char *host, uint16_t port, bool legacy)
{
    if (name == NULL || name[0] == '\0' || host == NULL || host[0] == '\0' || port == 0) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    for (int i = 0; i < s_entry_count; i++) {
        entry_t *entry = &s_entries[i];
        if (strcasecmp(entry->target.name, name) != 0) {
            continue;
        }
        bool changed = strcmp(entry->target.host, host) != 0 || entry->target.port != port ||
                       entry->target.legacy != legacy;
        strlcpy(entry->target.host, host, sizeof(entry->target.host));
        entry->target.port = port;
        entry->target.legacy = legacy;
        entry->via_airplay = true;
        entry->seen = xTaskGetTickCount();
        if (changed && legacy) {
            persist_legacy_locked();
        }
        xSemaphoreGive(s_lock);
        return;
    }
    if (s_entry_count < AP_DISCOVERY_MAX_TARGETS) {
        entry_t *entry = &s_entries[s_entry_count++];
        memset(entry, 0, sizeof(*entry));
        strlcpy(entry->target.name, name, sizeof(entry->target.name));
        strlcpy(entry->target.host, host, sizeof(entry->target.host));
        entry->target.port = port;
        entry->target.legacy = legacy;
        entry->via_airplay = true;
        entry->seen = xTaskGetTickCount();
        if (legacy) {
            persist_legacy_locked();
        }
        ESP_LOGI(TAG, "receiver \"%s\" at %s:%u remembered from a session", name, host,
                 (unsigned)port);
    }
    xSemaphoreGive(s_lock);
}

bool ap_discovery_forget(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    for (int i = 0; i < s_entry_count; i++) {
        if (strcasecmp(s_entries[i].target.name, name) != 0) {
            continue;
        }
        bool was_legacy = s_entries[i].target.legacy;
        s_entries[i] = s_entries[s_entry_count - 1];
        s_entry_count--;
        if (was_legacy) {
            persist_legacy_locked();
        }
        xSemaphoreGive(s_lock);
        return true;
    }
    xSemaphoreGive(s_lock);
    return false;
}

bool ap_discovery_find(const char *name, airplay_target_t *out)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (s_entry_count == 0) {
        ap_discovery_browse();
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    /* Entries are keyed by name, so an exact match is always unique. */
    for (int i = 0; i < s_entry_count; i++) {
        if (strcasecmp(s_entries[i].target.name, name) == 0) {
            *out = s_entries[i].target;
            xSemaphoreGive(s_lock);
            return true;
        }
    }

    char wanted[AP_DISCOVERY_NAME_LEN];
    strlcpy(wanted, name, sizeof(wanted));
    for (char *cursor = wanted; *cursor != '\0'; cursor++) {
        *cursor = (char)tolower((unsigned char)*cursor);
    }

    const entry_t *match = NULL;
    for (int i = 0; i < s_entry_count; i++) {
        char lower[AP_DISCOVERY_NAME_LEN];
        strlcpy(lower, s_entries[i].target.name, sizeof(lower));
        for (char *cursor = lower; *cursor != '\0'; cursor++) {
            *cursor = (char)tolower((unsigned char)*cursor);
        }
        if (strstr(lower, wanted) == NULL) {
            continue;
        }
        if (match != NULL) {
            ESP_LOGW(TAG, "\"%s\" matches several receivers; be more specific", name);
            xSemaphoreGive(s_lock);
            return false;
        }
        match = &s_entries[i];
    }
    if (match != NULL) {
        *out = match->target;
    }
    xSemaphoreGive(s_lock);
    return match != NULL;
}
