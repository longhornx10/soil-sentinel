#include "storage.h"

#include <string.h>
#include "esp_attr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NS "sentinel"
#define ZIGBEE_STORAGE_PARTITION "zb_storage"
#define RUNTIME_MAGIC 0x534F494Cu
#define RUNTIME_VERSION 5u
#define BUNDLE_MAGIC 0x42554E44u
#define BUNDLE_VERSION 5u
#define BUNDLE_KEY "bundle_v5"

#define LEGACY_POLICY_MAGIC 0x504F4C59u
#define LEGACY_STATE_MAGIC 0x53544154u
#define LEGACY_VERSION 4u
#define LEGACY_POLICY_KEY "policy_v4"
#define LEGACY_STATE_KEY "state_v4"

typedef struct {
    uint32_t magic;
    uint32_t version;
    soil_state_t state;
} retained_runtime_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    soil_policy_t policy;
    soil_state_t state;
} stored_bundle_t;

typedef struct {
    float dry_raw_mv;
    float wet_raw_mv;
    float dry_threshold_pct;
    float critical_threshold_pct;
    float report_delta_pct;
    float watering_delta_pct;
    float noise_fault_mv;
    uint32_t heartbeat_seconds;
    uint32_t stable_sample_seconds;
    uint32_t drying_sample_seconds;
    uint32_t near_dry_sample_seconds;
    uint32_t critical_sample_seconds;
    uint32_t watering_sample_seconds;
    uint32_t recent_water_sample_seconds;
} legacy_policy_value_t;

typedef struct {
    bool initialized;
    bool has_valid_moisture;
    bool current_sample_valid;
    bool battery_present;
    bool has_watered;
    float moisture_pct;
    float previous_moisture_pct;
    float last_reported_pct;
    float drying_rate_pct_per_hour;
    float confidence_pct;
    float battery_pct;
    uint32_t seconds_since_report;
    uint32_t seconds_since_watering;
    uint32_t sample_interval_seconds;
    uint32_t event_flags;
    uint64_t last_sample_rtc_us;
    soil_mode_t mode;
    bool sensor_fault;
    bool should_report;
} legacy_state_value_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    legacy_policy_value_t policy;
} legacy_policy_blob_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    legacy_state_value_t state;
} legacy_state_blob_t;

RTC_DATA_ATTR static retained_runtime_t s_runtime;

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

static void migrate_legacy_policy(const legacy_policy_value_t *legacy, soil_policy_t *policy)
{
    if (!legacy || !policy) return;
    policy->dry_raw_mv = legacy->dry_raw_mv;
    policy->wet_raw_mv = legacy->wet_raw_mv;
    policy->manual_dry_raw_mv = legacy->dry_raw_mv;
    policy->manual_wet_raw_mv = legacy->wet_raw_mv;
    policy->dry_threshold_pct = legacy->dry_threshold_pct;
    policy->critical_threshold_pct = legacy->critical_threshold_pct;
    policy->report_delta_pct = legacy->report_delta_pct;
    policy->watering_delta_pct = legacy->watering_delta_pct;
    policy->noise_fault_mv = legacy->noise_fault_mv;
    policy->heartbeat_seconds = legacy->heartbeat_seconds;
    policy->stable_sample_seconds = legacy->stable_sample_seconds;
    policy->drying_sample_seconds = legacy->drying_sample_seconds;
    policy->near_dry_sample_seconds = legacy->near_dry_sample_seconds;
    policy->critical_sample_seconds = legacy->critical_sample_seconds;
    policy->watering_sample_seconds = legacy->watering_sample_seconds;
    policy->recent_water_sample_seconds = legacy->recent_water_sample_seconds;
    policy->calibration_mode = SOIL_CALIBRATION_STOCK;
    policy->config_revision = 1U;
}

static void migrate_legacy_state(const legacy_state_value_t *legacy, soil_state_t *state)
{
    if (!legacy || !state) return;
    state->initialized = legacy->initialized;
    state->has_valid_moisture = legacy->has_valid_moisture;
    state->current_sample_valid = legacy->current_sample_valid;
    state->battery_present = legacy->battery_present;
    state->has_watered = legacy->has_watered;
    state->moisture_pct = legacy->moisture_pct;
    state->previous_moisture_pct = legacy->previous_moisture_pct;
    state->last_reported_pct = legacy->last_reported_pct;
    state->drying_rate_pct_per_hour = legacy->drying_rate_pct_per_hour;
    state->confidence_pct = legacy->confidence_pct;
    state->battery_pct = legacy->battery_pct;
    state->seconds_since_report = legacy->seconds_since_report;
    state->seconds_since_watering = legacy->seconds_since_watering;
    state->sample_interval_seconds = legacy->sample_interval_seconds;
    state->event_flags = legacy->event_flags;
    state->last_sample_rtc_us = legacy->last_sample_rtc_us;
    state->mode = legacy->mode;
    state->sensor_fault = legacy->sensor_fault;
    state->should_report = legacy->should_report;
}

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t storage_load(soil_policy_t *policy, soil_state_t *state)
{
    if (!policy || !state) return ESP_ERR_INVALID_ARG;
    *policy = soil_policy_default();
    memset(state, 0, sizeof(*state));

    stored_bundle_t bundle = {0};
    if (read_blob(BUNDLE_KEY, &bundle, sizeof(bundle)) == ESP_OK &&
        bundle.magic == BUNDLE_MAGIC && bundle.version == BUNDLE_VERSION) {
        soil_config_result_t result;
        if (soil_policy_validate(&bundle.policy, &result)) {
            *policy = bundle.policy;
            *state = bundle.state;
        }
    } else {
        legacy_policy_blob_t old_policy = {0};
        legacy_state_blob_t old_state = {0};
        if (read_blob(LEGACY_POLICY_KEY, &old_policy, sizeof(old_policy)) == ESP_OK &&
            old_policy.magic == LEGACY_POLICY_MAGIC &&
            old_policy.version == LEGACY_VERSION) {
            migrate_legacy_policy(&old_policy.policy, policy);
        }
        if (read_blob(LEGACY_STATE_KEY, &old_state, sizeof(old_state)) == ESP_OK &&
            old_state.magic == LEGACY_STATE_MAGIC &&
            old_state.version == LEGACY_VERSION) {
            migrate_legacy_state(&old_state.state, state);
        }
        state->applied_config_revision = policy->config_revision;
        soil_resolve_active_curve(policy, state);
        (void)storage_save_checkpoint(policy, state);
    }

    if (s_runtime.magic == RUNTIME_MAGIC && s_runtime.version == RUNTIME_VERSION) {
        /* RTC is the newest deep-sleep state, including learning updates that have not
         * yet reached the sparse NVS checkpoint. NVS remains the cold-boot fallback. */
        *state = s_runtime.state;
        state->applied_config_revision = policy->config_revision;
    }

    soil_resolve_active_curve(policy, state);
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

esp_err_t storage_save_checkpoint(const soil_policy_t *policy, const soil_state_t *state)
{
    if (!policy || !state) return ESP_ERR_INVALID_ARG;
    const stored_bundle_t bundle = {
        .magic = BUNDLE_MAGIC,
        .version = BUNDLE_VERSION,
        .policy = *policy,
        .state = *state,
    };
    return write_blob(BUNDLE_KEY, &bundle, sizeof(bundle));
}

esp_err_t storage_save_policy(const soil_policy_t *policy, const soil_state_t *state)
{
    return storage_save_checkpoint(policy, state);
}

esp_err_t storage_factory_reset(bool erase_zigbee_state)
{
    memset(&s_runtime, 0, sizeof(s_runtime));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_all(handle);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (erase_zigbee_state) {
        err = nvs_flash_erase_partition(ZIGBEE_STORAGE_PARTITION);
    }
    return err;
}
