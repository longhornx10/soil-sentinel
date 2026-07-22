#include "storage.h"

#include <string.h>
#include "esp_attr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NS "sentinel"
#define RUNTIME_MAGIC 0x534F494Cu
#define RUNTIME_VERSION 3u
#define POLICY_MAGIC 0x504F4C59u
#define POLICY_VERSION 3u
#define STATE_MAGIC 0x53544154u
#define STATE_VERSION 3u
#define POLICY_KEY "policy_v3"
#define STATE_KEY "state_v3"

typedef struct {
    uint32_t magic;
    uint32_t version;
    soil_state_t state;
} retained_runtime_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    soil_policy_t policy;
} stored_policy_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    soil_state_t state;
} stored_state_t;

RTC_DATA_ATTR static retained_runtime_t s_runtime;

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

    if (err != ESP_OK) return err;
    return size == expected ? ESP_OK : ESP_ERR_INVALID_SIZE;
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
    stored_policy_t stored_policy = {0};
    if (read_blob(POLICY_KEY, &stored_policy, sizeof(stored_policy)) == ESP_OK &&
        stored_policy.magic == POLICY_MAGIC && stored_policy.version == POLICY_VERSION) {
        *policy = stored_policy.policy;
    }

    if (s_runtime.magic == RUNTIME_MAGIC && s_runtime.version == RUNTIME_VERSION) {
        *state = s_runtime.state;
        return ESP_OK;
    }

    memset(state, 0, sizeof(*state));
    stored_state_t stored_state = {0};
    if (read_blob(STATE_KEY, &stored_state, sizeof(stored_state)) == ESP_OK &&
        stored_state.magic == STATE_MAGIC && stored_state.version == STATE_VERSION) {
        *state = stored_state.state;
    }
    storage_save_runtime(state);
    return ESP_OK;
}

void storage_save_runtime(const soil_state_t *state)
{
    if (!state) return;
    s_runtime.magic = RUNTIME_MAGIC;
    s_runtime.version = RUNTIME_VERSION;
    s_runtime.state = *state;
}

esp_err_t storage_save_policy(const soil_policy_t *policy)
{
    if (!policy) return ESP_ERR_INVALID_ARG;
    const stored_policy_t stored = {
        .magic = POLICY_MAGIC,
        .version = POLICY_VERSION,
        .policy = *policy,
    };
    return write_blob(POLICY_KEY, &stored, sizeof(stored));
}

esp_err_t storage_save_checkpoint(const soil_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    const stored_state_t stored = {
        .magic = STATE_MAGIC,
        .version = STATE_VERSION,
        .state = *state,
    };
    return write_blob(STATE_KEY, &stored, sizeof(stored));
}
