#include "storage.h"

#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

#define NS "sentinel"

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t read_blob(const char *key, void *value, size_t expected)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t size = expected;
    err = nvs_get_blob(handle, key, value, &size);
    nvs_close(handle);
    return (err == ESP_OK && size == expected) ? ESP_OK : err;
}

static esp_err_t write_blob(const char *key, const void *value, size_t size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, key, value, size);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t storage_load(soil_policy_t *policy, soil_state_t *state)
{
    if (!policy || !state) return ESP_ERR_INVALID_ARG;
    *policy = soil_policy_default();
    memset(state, 0, sizeof(*state));
    (void)read_blob("policy", policy, sizeof(*policy));
    (void)read_blob("state", state, sizeof(*state));
    return ESP_OK;
}

esp_err_t storage_save_policy(const soil_policy_t *policy)
{
    return policy ? write_blob("policy", policy, sizeof(*policy)) : ESP_ERR_INVALID_ARG;
}

esp_err_t storage_save_checkpoint(const soil_state_t *state)
{
    return state ? write_blob("state", state, sizeof(*state)) : ESP_ERR_INVALID_ARG;
}
