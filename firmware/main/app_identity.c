#include "app_identity.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "identity";
static const char *NAMESPACE = "airplay";
static const char *KEY_SEED = "id_seed";
static const char *KEY_UUID = "id_uuid";

esp_err_t app_identity_load(hap_identity_t *identity)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t seed[HAP_ED25519_SEED_LEN];
    size_t seed_len = sizeof(seed);
    char uuid[HAP_ID_LEN + 1];
    size_t uuid_len = sizeof(uuid);

    err = nvs_get_blob(handle, KEY_SEED, seed, &seed_len);
    esp_err_t uuid_err = nvs_get_str(handle, KEY_UUID, uuid, &uuid_len);
    if (err == ESP_OK && uuid_err == ESP_OK && seed_len == sizeof(seed)) {
        nvs_close(handle);
        if (hap_identity_from_seed(identity, seed, uuid) != 0) {
            ESP_LOGE(TAG, "stored identity is unusable, generating a new one");
        } else {
            ESP_LOGI(TAG, "loaded identity %s", identity->pairing_id);
            return ESP_OK;
        }
    }

    hap_identity_generate(identity);
    err = nvs_set_blob(handle, KEY_SEED, identity->seed, sizeof(identity->seed));
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_UUID, identity->pairing_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not persist the identity: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "created identity %s", identity->pairing_id);
    return ESP_OK;
}
