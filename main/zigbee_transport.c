#include "zigbee_transport.h"

#include <math.h>
#include <string.h>
#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_zigbee.h"
#include "firmware_update.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "storage.h"
#include "soil_telemetry.h"
#include "ezbee/af.h"
#include "ezbee/app_signals.h"
#include "ezbee/bdb.h"
#include "ezbee/nwk.h"
#include "ezbee/zcl/zcl_common.h"
#include "ezbee/zcl/zcl_core.h"
#include "ezbee/zcl/zcl_general_cmd.h"
#include "ezbee/zcl/cluster/analog_input_desc.h"
#include "ezbee/zcl/cluster/basic_desc.h"
#include "ezbee/zcl/cluster/custom.h"
#include "ezbee/zcl/cluster/identify_desc.h"
#include "ezbee/zcl/cluster/ota_upgrade.h"
#include "ezbee/zcl/cluster/power_config_desc.h"

#define ENDPOINT_ID 1U
#define CUSTOM_DEVICE_ID 0xFFF0U
#define TELEMETRY_CLUSTER_ID 0xFC00U
#define TELEMETRY_ATTR_ID 0x0000U
#define CONTROL_CLUSTER_ID 0xFC01U

#define CTRL_ATTR_DESIRED_MODE          0x0000U
#define CTRL_ATTR_DESIRED_MANUAL_DRY    0x0001U
#define CTRL_ATTR_DESIRED_MANUAL_WET    0x0002U
#define CTRL_ATTR_DESIRED_DRY_THRESHOLD 0x0003U
#define CTRL_ATTR_DESIRED_CRIT_THRESHOLD 0x0004U
#define CTRL_ATTR_DESIRED_REVISION      0x0005U
#define CTRL_ATTR_ACTION                0x0006U
#define CTRL_ATTR_APPLIED_REVISION      0x0010U
#define CTRL_ATTR_CONFIG_RESULT         0x0011U
#define CTRL_ATTR_ACTIVE_MODE           0x0012U
#define CTRL_ATTR_ACTIVE_DRY            0x0013U
#define CTRL_ATTR_ACTIVE_WET            0x0014U
#define CTRL_ATTR_LEARNED_DRY           0x0015U
#define CTRL_ATTR_LEARNED_WET           0x0016U
#define CTRL_ATTR_LEARNING_CONFIDENCE   0x0017U
#define CTRL_ATTR_LEARNING_CYCLES       0x0018U

#define TELEMETRY_SCHEMA_VERSION 2U
#define TELEMETRY_PAYLOAD_LENGTH 48U
#define TELEMETRY_BUFFER_LENGTH (TELEMETRY_PAYLOAD_LENGTH + 1U)

#define READY_BIT BIT0
#define REPORT_MOISTURE_CONFIRMED_BIT BIT1
#define REPORT_MOISTURE_FAILED_BIT BIT2
#define REPORT_BATTERY_PERCENT_CONFIRMED_BIT BIT3
#define REPORT_BATTERY_PERCENT_FAILED_BIT BIT4
#define REPORT_BATTERY_VOLTAGE_CONFIRMED_BIT BIT5
#define REPORT_BATTERY_VOLTAGE_FAILED_BIT BIT6
#define REPORT_TELEMETRY_CONFIRMED_BIT BIT7
#define REPORT_TELEMETRY_FAILED_BIT BIT8
#define CONFIG_CHANGED_BIT BIT9
#define IDENTIFY_REQUEST_BIT BIT10

#define PRIMARY_CHANNEL_MASK 0x07FFF800UL
#define ZIGBEE_STORAGE_PARTITION "zb_storage"
#define COMMISSIONING_RETRY_DELAY_MS 1000U
#define REPORT_CONFIRM_TIMEOUT_MS 3000U
#define OPTIONAL_REPORT_CONFIRM_TIMEOUT_MS 1000U
#define COORDINATOR_SHORT_ADDRESS 0x0000U
#define COORDINATOR_ENDPOINT 1U
#define AF_CONFIRM_SUCCESS 0x00U
#define ZIGBEE_LOCK_TIMEOUT_MS 2000U

static const char *TAG = "zigbee";
static EventGroupHandle_t s_events;
static volatile bool s_retry_pending;
static ezb_bdb_comm_mode_mask_t s_retry_mode;
static bool s_ota_mode;
static soil_policy_t *s_policy;
static soil_state_t *s_state;
static soil_diagnostics_t *s_diagnostics;

static uint8_t s_telemetry[TELEMETRY_BUFFER_LENGTH] = {
    TELEMETRY_PAYLOAD_LENGTH, TELEMETRY_SCHEMA_VERSION
};

static uint8_t s_desired_mode;
static uint16_t s_desired_manual_dry;
static uint16_t s_desired_manual_wet;
static uint8_t s_desired_dry_threshold;
static uint8_t s_desired_critical_threshold;
static uint32_t s_desired_revision;
static uint16_t s_action;
static uint32_t s_applied_revision;
static uint8_t s_config_result;
static uint8_t s_active_mode;
static uint16_t s_active_dry;
static uint16_t s_active_wet;
static uint16_t s_learned_dry;
static uint16_t s_learned_wet;
static uint8_t s_learning_confidence;
static uint16_t s_learning_cycles;

static float s_attr_min_value = 0.0f;
static float s_attr_max_value = 100.0f;
static float s_attr_resolution = 0.1f;
static uint16_t s_attr_engineering_units = 98U;
static uint8_t s_attr_battery_voltage = EZB_ZCL_VALUE_UINT8_FF;
static uint8_t s_attr_battery_percentage = EZB_ZCL_VALUE_UINT8_FF;
static uint8_t s_attr_battery_size = EZB_ZCL_POWER_CONFIG_BATTERY_SIZE_AA;
static uint8_t s_attr_battery_quantity = 1U;
static uint8_t s_attr_battery_rated_voltage = 15U;
static uint8_t s_sw_build_id[33] = {0};
static uint8_t s_manufacturer_name[sizeof("longhornx10")] = {0};
static uint8_t s_model_identifier[sizeof("Soil Sentinel")] = {0};
static uint8_t s_moisture_description[sizeof("Soil moisture")] = {0};

typedef struct {
    EventBits_t success_bit;
    EventBits_t failure_bit;
    const char *name;
} report_confirm_context_t;

static report_confirm_context_t s_moisture_confirm = {
    .success_bit = REPORT_MOISTURE_CONFIRMED_BIT,
    .failure_bit = REPORT_MOISTURE_FAILED_BIT,
    .name = "moisture",
};
static report_confirm_context_t s_battery_percent_confirm = {
    .success_bit = REPORT_BATTERY_PERCENT_CONFIRMED_BIT,
    .failure_bit = REPORT_BATTERY_PERCENT_FAILED_BIT,
    .name = "battery percentage",
};
static report_confirm_context_t s_battery_voltage_confirm = {
    .success_bit = REPORT_BATTERY_VOLTAGE_CONFIRMED_BIT,
    .failure_bit = REPORT_BATTERY_VOLTAGE_FAILED_BIT,
    .name = "battery voltage",
};
static report_confirm_context_t s_telemetry_confirm = {
    .success_bit = REPORT_TELEMETRY_CONFIRMED_BIT,
    .failure_bit = REPORT_TELEMETRY_FAILED_BIT,
    .name = "telemetry",
};

static bool init_zcl_string(uint8_t *destination,
                            size_t capacity,
                            const char *value)
{
    if (!destination || !value || capacity == 0U) return false;
    const size_t length = strlen(value);
    if (length > UINT8_MAX || length + 1U > capacity) return false;
    destination[0] = (uint8_t)length;
    memcpy(&destination[1], value, length);
    return true;
}

static uint16_t clamp_u16(float value)
{
    if (!isfinite(value) || value <= 0.0f) return 0U;
    if (value >= 65535.0f) return UINT16_MAX;
    return (uint16_t)(value + 0.5f);
}

static int16_t clamp_i16(float value)
{
    if (!isfinite(value)) return 0;
    if (value <= (float)INT16_MIN) return INT16_MIN;
    if (value >= (float)INT16_MAX) return INT16_MAX;
    return (int16_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

static uint8_t clamp_u8(float value)
{
    if (!isfinite(value) || value <= 0.0f) return 0U;
    if (value >= 255.0f) return UINT8_MAX;
    return (uint8_t)(value + 0.5f);
}

static void put_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xffU);
    dst[1] = (uint8_t)(value >> 8U);
}

static void put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xffU);
    dst[1] = (uint8_t)((value >> 8U) & 0xffU);
    dst[2] = (uint8_t)((value >> 16U) & 0xffU);
    dst[3] = (uint8_t)((value >> 24U) & 0xffU);
}

static void refresh_control_values(void)
{
    if (!s_policy || !s_state) return;
    s_applied_revision = s_policy->config_revision;
    s_config_result = (uint8_t)s_state->last_config_result;
    s_active_mode = (uint8_t)s_policy->calibration_mode;
    s_active_dry = clamp_u16(s_state->active_dry_raw_mv);
    s_active_wet = clamp_u16(s_state->active_wet_raw_mv);
    s_learned_dry = clamp_u16(s_state->learned_dry_raw_mv);
    s_learned_wet = clamp_u16(s_state->learned_wet_raw_mv);
    s_learning_confidence = clamp_u8(s_state->learning_confidence_pct);
    s_learning_cycles = s_state->learning_cycle_count > UINT16_MAX
                            ? UINT16_MAX
                            : (uint16_t)s_state->learning_cycle_count;
}

static void encode_telemetry(const soil_state_t *state,
                             const soil_diagnostics_t *diag)
{
    memset(s_telemetry, 0, sizeof(s_telemetry));
    s_telemetry[0] = TELEMETRY_PAYLOAD_LENGTH;
    s_telemetry[1] = TELEMETRY_SCHEMA_VERSION;
    s_telemetry[2] = (uint8_t)state->mode;
    put_u16_le(&s_telemetry[3], soil_telemetry_flags(state));
    s_telemetry[5] = state->sensor_fault ? 1U : 0U;
    s_telemetry[6] = clamp_u8(state->confidence_pct);
    put_u16_le(&s_telemetry[7], clamp_u16(diag->raw_mv));
    put_u16_le(&s_telemetry[9], clamp_u16(diag->noise_mv));
    put_u16_le(&s_telemetry[11], (uint16_t)clamp_i16(state->drying_rate_pct_per_hour * 100.0f));
    put_u32_le(&s_telemetry[13], state->sample_interval_seconds);
    put_u32_le(&s_telemetry[17], state->seconds_since_watering);
    s_telemetry[21] = state->battery_present ? 1U : 0U;
    s_telemetry[22] = (uint8_t)s_policy->calibration_mode;
    s_telemetry[23] = (uint8_t)state->active_curve_source;
    s_telemetry[24] = (uint8_t)state->last_config_result;
    put_u16_le(&s_telemetry[25], clamp_u16(state->active_dry_raw_mv));
    put_u16_le(&s_telemetry[27], clamp_u16(state->active_wet_raw_mv));
    put_u16_le(&s_telemetry[29], clamp_u16(state->learned_dry_raw_mv));
    put_u16_le(&s_telemetry[31], clamp_u16(state->learned_wet_raw_mv));
    s_telemetry[33] = clamp_u8(state->learning_confidence_pct);
    put_u16_le(&s_telemetry[34], state->learning_cycle_count > UINT16_MAX
                                      ? UINT16_MAX
                                      : (uint16_t)state->learning_cycle_count);
    put_u32_le(&s_telemetry[36], state->applied_config_revision);
    s_telemetry[40] = (uint8_t)firmware_update_state();
    s_telemetry[41] = (uint8_t)firmware_update_last_result();
    s_telemetry[42] = firmware_update_progress_percent();
    s_telemetry[43] = firmware_update_active_slot();
    s_telemetry[44] = firmware_update_rollback_state();
    put_u32_le(&s_telemetry[45], firmware_update_file_version());
}

static esp_err_t init_zigbee_storage(void)
{
    esp_err_t err = nvs_flash_init_partition(ZIGBEE_STORAGE_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase_partition(ZIGBEE_STORAGE_PARTITION), TAG,
                            "failed to erase Zigbee NVS partition");
        err = nvs_flash_init_partition(ZIGBEE_STORAGE_PARTITION);
    }
    return err;
}

static bool acquire_zigbee_lock(const char *operation)
{
    if (esp_zigbee_lock_acquire(pdMS_TO_TICKS(ZIGBEE_LOCK_TIMEOUT_MS))) {
        return true;
    }
    ESP_LOGE(TAG, "timed out acquiring Zigbee lock for %s", operation);
    return false;
}

static ezb_err_t start_commissioning(ezb_bdb_comm_mode_mask_t mode,
                                     const char *reason)
{
    const ezb_err_t err = ezb_bdb_start_top_level_commissioning(mode);
    if (err == EZB_ERR_NONE) {
        ESP_LOGI(TAG, "started BDB commissioning mode=0x%02x (%s)",
                 (unsigned)mode, reason);
    } else {
        ESP_LOGE(TAG, "failed BDB commissioning mode=0x%02x (%s): 0x%04x",
                 (unsigned)mode, reason, (unsigned)err);
    }
    return err;
}

static void commissioning_retry_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(COMMISSIONING_RETRY_DELAY_MS));
    const ezb_bdb_comm_mode_mask_t mode = s_retry_mode;
    if (!acquire_zigbee_lock("commissioning retry")) {
        s_retry_pending = false;
        vTaskDelete(NULL);
        return;
    }
    const ezb_err_t err = start_commissioning(mode, "scheduled retry");
    esp_zigbee_lock_release();
    s_retry_pending = false;
    if (err != EZB_ERR_NONE) ESP_LOGW(TAG, "commissioning retry rejected");
    vTaskDelete(NULL);
}

static void schedule_commissioning_retry(ezb_bdb_comm_mode_mask_t mode)
{
    if (s_retry_pending) return;
    s_retry_mode = mode;
    s_retry_pending = true;
    if (xTaskCreate(commissioning_retry_task, "zb_retry", 3072, NULL, 4, NULL) != pdPASS) {
        s_retry_pending = false;
        ESP_LOGE(TAG, "failed to create commissioning retry task");
    }
}

static bool app_signal_handler(const ezb_app_signal_t *signal)
{
    const ezb_app_signal_type_t type = ezb_app_signal_get_type(signal);
    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        (void)start_commissioning(EZB_BDB_MODE_INITIALIZATION, "stack startup");
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        const ezb_bdb_comm_status_t status =
            *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            if (ezb_bdb_is_factory_new()) {
                (void)board_pairing_indicator_start();
                (void)start_commissioning(EZB_BDB_MODE_NETWORK_STEERING,
                                          "factory-new device");
            } else {
                xEventGroupSetBits(s_events, READY_BIT);
            }
        } else {
            schedule_commissioning_retry(EZB_BDB_MODE_INITIALIZATION);
        }
        break;
    }
    case EZB_BDB_SIGNAL_STEERING: {
        const ezb_bdb_comm_status_t status =
            *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            board_pairing_indicator_success();
            xEventGroupSetBits(s_events, READY_BIT);
        } else {
            schedule_commissioning_retry(EZB_BDB_MODE_NETWORK_STEERING);
        }
        break;
    }
    default:
        break;
    }
    return true;
}

static ezb_zcl_status_t set_cluster_attr(uint16_t attr_id, void *value)
{
    return ezb_zcl_set_attr_value(ENDPOINT_ID, CONTROL_CLUSTER_ID,
                                  EZB_ZCL_CLUSTER_SERVER, attr_id,
                                  EZB_ZCL_STD_MANUF_CODE, value, false);
}

static void sync_control_cluster(void)
{
    refresh_control_values();
    (void)set_cluster_attr(CTRL_ATTR_APPLIED_REVISION, &s_applied_revision);
    (void)set_cluster_attr(CTRL_ATTR_CONFIG_RESULT, &s_config_result);
    (void)set_cluster_attr(CTRL_ATTR_ACTIVE_MODE, &s_active_mode);
    (void)set_cluster_attr(CTRL_ATTR_ACTIVE_DRY, &s_active_dry);
    (void)set_cluster_attr(CTRL_ATTR_ACTIVE_WET, &s_active_wet);
    (void)set_cluster_attr(CTRL_ATTR_LEARNED_DRY, &s_learned_dry);
    (void)set_cluster_attr(CTRL_ATTR_LEARNED_WET, &s_learned_wet);
    (void)set_cluster_attr(CTRL_ATTR_LEARNING_CONFIDENCE, &s_learning_confidence);
    (void)set_cluster_attr(CTRL_ATTR_LEARNING_CYCLES, &s_learning_cycles);
}

static soil_config_result_t persist_candidate(const soil_policy_t *candidate)
{
    const soil_policy_t old_policy = *s_policy;
    const soil_state_t old_state = *s_state;
    *s_policy = *candidate;
    s_state->applied_config_revision = candidate->config_revision;
    s_state->last_config_result = SOIL_CONFIG_OK;
    s_state->event_flags |= SOIL_EVENT_CONFIG;
    s_state->should_report = true;
    s_state->seconds_since_report = 0U;
    soil_resolve_active_curve(s_policy, s_state);
    if (storage_save_policy(s_policy, s_state) != ESP_OK) {
        *s_policy = old_policy;
        *s_state = old_state;
        s_state->last_config_result = SOIL_CONFIG_REJECT_PERSISTENCE;
        return SOIL_CONFIG_REJECT_PERSISTENCE;
    }
    storage_save_runtime(s_state);
    xEventGroupSetBits(s_events, CONFIG_CHANGED_BIT);
    return SOIL_CONFIG_OK;
}

static soil_config_result_t apply_desired_configuration(void)
{
    if (!s_policy || !s_state) return SOIL_CONFIG_REJECT_PERSISTENCE;
    if (s_desired_revision <= s_policy->config_revision) return SOIL_CONFIG_OK;

    soil_policy_t candidate = *s_policy;
    const soil_config_result_t result = soil_policy_apply_configuration(
        &candidate,
        (soil_calibration_mode_t)s_desired_mode,
        (float)s_desired_manual_dry,
        (float)s_desired_manual_wet,
        (float)s_desired_dry_threshold,
        (float)s_desired_critical_threshold,
        s_desired_revision);
    if (result != SOIL_CONFIG_OK) {
        s_state->last_config_result = result;
        xEventGroupSetBits(s_events, CONFIG_CHANGED_BIT);
        return result;
    }
    return persist_candidate(&candidate);
}

static soil_config_result_t apply_action(uint16_t action_bits)
{
    if (!s_policy || !s_state) return SOIL_CONFIG_REJECT_PERSISTENCE;
    if (action_bits & SOIL_ACTION_IDENTIFY) {
        xEventGroupSetBits(s_events, IDENTIFY_REQUEST_BIT);
    }

    const uint16_t stateful = action_bits & (SOIL_ACTION_USE_CURRENT_AS_DRY |
                                             SOIL_ACTION_USE_CURRENT_AS_WET |
                                             SOIL_ACTION_COPY_LEARNED_TO_MANUAL |
                                             SOIL_ACTION_RESET_LEARNING |
                                             SOIL_ACTION_RESTORE_MANUAL_STOCK |
                                             SOIL_ACTION_PLANT_MOVED);
    if (!stateful) return SOIL_CONFIG_OK;

    const soil_policy_t old_policy = *s_policy;
    const soil_state_t old_state = *s_state;
    soil_config_result_t result = SOIL_CONFIG_OK;
    const float current_raw = s_diagnostics && s_state->current_sample_valid
                                  ? s_diagnostics->raw_mv
                                  : NAN;
    for (uint16_t bit = 1U; bit <= SOIL_ACTION_PLANT_MOVED; bit <<= 1U) {
        if ((stateful & bit) == 0U) continue;
        result = soil_apply_control_action(s_policy, s_state,
                                           (soil_control_action_t)bit,
                                           current_raw);
        if (result != SOIL_CONFIG_OK) break;
    }

    if (result == SOIL_CONFIG_OK && storage_save_policy(s_policy, s_state) != ESP_OK) {
        result = SOIL_CONFIG_REJECT_PERSISTENCE;
    }
    if (result != SOIL_CONFIG_OK) {
        *s_policy = old_policy;
        *s_state = old_state;
        s_state->last_config_result = result;
    } else {
        storage_save_runtime(s_state);
    }
    xEventGroupSetBits(s_events, CONFIG_CHANGED_BIT);
    return result;
}

static void set_attr_handler(ezb_zcl_set_attr_value_message_t *message)
{
    if (!message) return;
    message->out.result = EZB_ZCL_STATUS_SUCCESS;
    if (message->info.status != EZB_ZCL_STATUS_SUCCESS ||
        message->info.dst_ep != ENDPOINT_ID ||
        message->info.cluster_id != CONTROL_CLUSTER_ID ||
        message->info.cluster_role != EZB_ZCL_CLUSTER_SERVER ||
        !message->in.attribute.data.value) {
        return;
    }

    switch (message->in.attribute.id) {
    case CTRL_ATTR_DESIRED_MODE:
        s_desired_mode = *(uint8_t *)message->in.attribute.data.value;
        break;
    case CTRL_ATTR_DESIRED_MANUAL_DRY:
        s_desired_manual_dry = *(uint16_t *)message->in.attribute.data.value;
        break;
    case CTRL_ATTR_DESIRED_MANUAL_WET:
        s_desired_manual_wet = *(uint16_t *)message->in.attribute.data.value;
        break;
    case CTRL_ATTR_DESIRED_DRY_THRESHOLD:
        s_desired_dry_threshold = *(uint8_t *)message->in.attribute.data.value;
        break;
    case CTRL_ATTR_DESIRED_CRIT_THRESHOLD:
        s_desired_critical_threshold = *(uint8_t *)message->in.attribute.data.value;
        break;
    case CTRL_ATTR_DESIRED_REVISION: {
        s_desired_revision = *(uint32_t *)message->in.attribute.data.value;
        const soil_config_result_t result = apply_desired_configuration();
        message->out.result = result == SOIL_CONFIG_OK
                                  ? EZB_ZCL_STATUS_SUCCESS
                                  : EZB_ZCL_STATUS_INVALID_VALUE;
        sync_control_cluster();
        break;
    }
    case CTRL_ATTR_ACTION: {
        s_action = *(uint16_t *)message->in.attribute.data.value;
        const soil_config_result_t result = apply_action(s_action);
        message->out.result = result == SOIL_CONFIG_OK
                                  ? EZB_ZCL_STATUS_SUCCESS
                                  : EZB_ZCL_STATUS_INVALID_VALUE;
        s_action = 0U;
        (void)set_cluster_attr(CTRL_ATTR_ACTION, &s_action);
        sync_control_cluster();
        break;
    }
    default:
        break;
    }
}

static void core_action_handler(ezb_zcl_core_action_callback_id_t callback_id,
                                void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID:
        set_attr_handler((ezb_zcl_set_attr_value_message_t *)message);
        break;
    case EZB_ZCL_CORE_OTA_UPGRADE_CLIENT_PROGRESS_CB_ID:
        if (s_ota_mode) {
            firmware_update_handle_progress(
                (ezb_zcl_ota_upgrade_client_progress_message_t *)message);
        } else if (message) {
            ((ezb_zcl_ota_upgrade_client_progress_message_t *)message)->out.result =
                EZB_ZCL_STATUS_ABORT;
        }
        break;
    case EZB_ZCL_CORE_OTA_UPGRADE_QUERY_NEXT_IMAGE_RSP_CB_ID:
        if (s_ota_mode) {
            firmware_update_handle_query_response(
                (ezb_zcl_ota_upgrade_query_next_image_rsp_message_t *)message);
        }
        break;
    default:
        break;
    }
}

static void telemetry_cluster_init(uint8_t ep_id) { (void)ep_id; }
static void control_cluster_init(uint8_t ep_id) { (void)ep_id; }

static esp_err_t add_control_attr(ezb_zcl_cluster_desc_t cluster,
                                  uint16_t id,
                                  uint8_t type,
                                  uint8_t access,
                                  void *value)
{
    return esp_zigbee_err_to_esp(ezb_zcl_custom_cluster_desc_add_attr(
        cluster, id, type, access, value));
}

static void initialize_desired_values(void)
{
    s_desired_mode = (uint8_t)s_policy->calibration_mode;
    s_desired_manual_dry = clamp_u16(s_policy->manual_dry_raw_mv);
    s_desired_manual_wet = clamp_u16(s_policy->manual_wet_raw_mv);
    s_desired_dry_threshold = clamp_u8(s_policy->dry_threshold_pct);
    s_desired_critical_threshold = clamp_u8(s_policy->critical_threshold_pct);
    s_desired_revision = s_policy->config_revision;
    s_action = 0U;
    refresh_control_values();

    const char *build = firmware_update_build_id();
    size_t length = 0U;
    while (length < sizeof(s_sw_build_id) - 1U && build[length] != '\0') {
        ++length;
    }
    s_sw_build_id[0] = (uint8_t)length;
    memcpy(&s_sw_build_id[1], build, length);
}

static esp_err_t create_device(void)
{
    initialize_desired_values();
    ESP_RETURN_ON_FALSE(
        init_zcl_string(s_manufacturer_name, sizeof(s_manufacturer_name),
                        "longhornx10"),
        ESP_ERR_INVALID_SIZE, TAG, "manufacturer string is too long");
    ESP_RETURN_ON_FALSE(
        init_zcl_string(s_model_identifier, sizeof(s_model_identifier),
                        "Soil Sentinel"),
        ESP_ERR_INVALID_SIZE, TAG, "model string is too long");
    ESP_RETURN_ON_FALSE(
        init_zcl_string(s_moisture_description,
                        sizeof(s_moisture_description),
                        "Soil moisture"),
        ESP_ERR_INVALID_SIZE, TAG, "description string is too long");
    ezb_af_device_desc_t device = ezb_af_create_device_desc();
    ESP_RETURN_ON_FALSE(device, ESP_ERR_NO_MEM, TAG, "device descriptor allocation failed");
    ezb_af_ep_config_t ep_cfg = {
        .ep_id = ENDPOINT_ID,
        .app_profile_id = EZB_AF_HA_PROFILE_ID,
        .app_device_id = CUSTOM_DEVICE_ID,
        .app_device_version = 3U,
    };
    ezb_af_ep_desc_t endpoint = ezb_af_create_endpoint_desc(&ep_cfg);
    ESP_RETURN_ON_FALSE(endpoint, ESP_ERR_NO_MEM, TAG, "endpoint allocation failed");

    ezb_zcl_basic_cluster_server_config_t basic_cfg = {
        .zcl_version = EZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = EZB_ZCL_BASIC_POWER_SOURCE_BATTERY,
    };
    ezb_zcl_cluster_desc_t basic = ezb_zcl_basic_create_cluster_desc(
        &basic_cfg, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic,
        EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, s_manufacturer_name);
    ezb_zcl_basic_cluster_desc_add_attr(basic,
        EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, s_model_identifier);
    ezb_zcl_basic_cluster_desc_add_attr(basic,
        EZB_ZCL_ATTR_BASIC_SW_BUILD_ID, s_sw_build_id);

    ezb_zcl_identify_cluster_server_config_t identify_cfg = {.identify_time = 0U};
    ezb_zcl_cluster_desc_t identify = ezb_zcl_identify_create_cluster_desc(
        &identify_cfg, EZB_ZCL_CLUSTER_SERVER);

    ezb_zcl_analog_input_cluster_server_config_t moisture_cfg = {
        .out_of_service = false,
        .present_value = 0.0f,
        .status_flags = 0U,
    };
    ezb_zcl_cluster_desc_t moisture = ezb_zcl_analog_input_create_cluster_desc(
        &moisture_cfg, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_analog_input_cluster_desc_add_attr(moisture,
        EZB_ZCL_ATTR_ANALOG_INPUT_MIN_PRESENT_VALUE_ID, &s_attr_min_value);
    ezb_zcl_analog_input_cluster_desc_add_attr(moisture,
        EZB_ZCL_ATTR_ANALOG_INPUT_MAX_PRESENT_VALUE_ID, &s_attr_max_value);
    ezb_zcl_analog_input_cluster_desc_add_attr(moisture,
        EZB_ZCL_ATTR_ANALOG_INPUT_RESOLUTION_ID, &s_attr_resolution);
    ezb_zcl_analog_input_cluster_desc_add_attr(moisture,
        EZB_ZCL_ATTR_ANALOG_INPUT_ENGINEERING_UNITS_ID, &s_attr_engineering_units);
    ezb_zcl_analog_input_cluster_desc_add_attr(moisture,
        EZB_ZCL_ATTR_ANALOG_INPUT_DESCRIPTION_ID, s_moisture_description);

    ezb_zcl_cluster_desc_t power = ezb_zcl_power_config_create_cluster_desc(
        NULL, EZB_ZCL_CLUSTER_SERVER);
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_zcl_power_config_cluster_desc_add_attr(
        power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
        &s_attr_battery_voltage)));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_zcl_power_config_cluster_desc_add_attr(
        power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
        &s_attr_battery_percentage)));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_zcl_power_config_cluster_desc_add_attr(
        power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_SIZE_ID,
        &s_attr_battery_size)));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_zcl_power_config_cluster_desc_add_attr(
        power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_QUANTITY_ID,
        &s_attr_battery_quantity)));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_zcl_power_config_cluster_desc_add_attr(
        power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_RATED_VOLTAGE_ID,
        &s_attr_battery_rated_voltage)));

    ezb_zcl_custom_cluster_config_t telemetry_cfg = {
        .cluster_id = TELEMETRY_CLUSTER_ID,
        .init_func = telemetry_cluster_init,
        .deinit_func = NULL,
    };
    ezb_zcl_cluster_desc_t telemetry = ezb_zcl_custom_create_cluster_desc(
        &telemetry_cfg, EZB_ZCL_CLUSTER_SERVER);
    ESP_ERROR_CHECK(add_control_attr(telemetry, TELEMETRY_ATTR_ID,
        EZB_ZCL_ATTR_TYPE_OCTSTR,
        EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING,
        s_telemetry));

    ezb_zcl_custom_cluster_config_t control_cfg = {
        .cluster_id = CONTROL_CLUSTER_ID,
        .init_func = control_cluster_init,
        .deinit_func = NULL,
    };
    ezb_zcl_cluster_desc_t control = ezb_zcl_custom_create_cluster_desc(
        &control_cfg, EZB_ZCL_CLUSTER_SERVER);
    const uint8_t rw = EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE;
    const uint8_t rp = EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING;
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_DESIRED_MODE,
        EZB_ZCL_ATTR_TYPE_ENUM8, rw, &s_desired_mode));
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_DESIRED_MANUAL_DRY,
        EZB_ZCL_ATTR_TYPE_UINT16, rw, &s_desired_manual_dry));
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_DESIRED_MANUAL_WET,
        EZB_ZCL_ATTR_TYPE_UINT16, rw, &s_desired_manual_wet));
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_DESIRED_DRY_THRESHOLD,
        EZB_ZCL_ATTR_TYPE_UINT8, rw, &s_desired_dry_threshold));
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_DESIRED_CRIT_THRESHOLD,
        EZB_ZCL_ATTR_TYPE_UINT8, rw, &s_desired_critical_threshold));
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_DESIRED_REVISION,
        EZB_ZCL_ATTR_TYPE_UINT32, rw, &s_desired_revision));
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_ACTION,
        EZB_ZCL_ATTR_TYPE_BITMAP16, rw, &s_action));
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_APPLIED_REVISION,
        EZB_ZCL_ATTR_TYPE_UINT32, rp, &s_applied_revision));
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_CONFIG_RESULT,
        EZB_ZCL_ATTR_TYPE_ENUM8, rp, &s_config_result));
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_ACTIVE_MODE,
        EZB_ZCL_ATTR_TYPE_ENUM8, rp, &s_active_mode));
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_ACTIVE_DRY,
        EZB_ZCL_ATTR_TYPE_UINT16, rp, &s_active_dry));
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_ACTIVE_WET,
        EZB_ZCL_ATTR_TYPE_UINT16, rp, &s_active_wet));
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_LEARNED_DRY,
        EZB_ZCL_ATTR_TYPE_UINT16, rp, &s_learned_dry));
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_LEARNED_WET,
        EZB_ZCL_ATTR_TYPE_UINT16, rp, &s_learned_wet));
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_LEARNING_CONFIDENCE,
        EZB_ZCL_ATTR_TYPE_UINT8, rp, &s_learning_confidence));
    ESP_ERROR_CHECK(add_control_attr(control, CTRL_ATTR_LEARNING_CYCLES,
        EZB_ZCL_ATTR_TYPE_UINT16, rp, &s_learning_cycles));

    ezb_zcl_ota_upgrade_cluster_client_config_t ota_cfg = {
        .upgrade_server_id = EZB_ZCL_OTA_UPGRADE_UPGRADE_SERVER_ID_DEFAULT_VALUE,
        .file_offset = 0U,
        .image_upgrade_status = EZB_ZCL_OTA_UPGRADE_IMAGE_UPGRADE_STATUS_DEFAULT_VALUE,
        .manufacturer_id = SOIL_OTA_MANUFACTURER_CODE,
        .image_type_id = SOIL_OTA_IMAGE_TYPE,
    };
    ezb_zcl_cluster_desc_t ota = ezb_zcl_ota_upgrade_create_cluster_desc(
        &ota_cfg, EZB_ZCL_CLUSTER_CLIENT);

    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, basic));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, identify));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, moisture));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, power));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, telemetry));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, control));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, ota));
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(device, endpoint));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(device));
    ezb_zcl_ota_upgrade_set_download_block_size(ENDPOINT_ID, 223U);
    ezb_zcl_core_action_handler_register(core_action_handler);
    return ESP_OK;
}

static void zigbee_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(init_zigbee_storage());
    esp_zigbee_config_t config = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE,
            .install_code_policy = false,
            .zed_config = {
                .ed_timeout = EZB_NWK_ED_TIMEOUT_16384MIN,
                .keep_alive = 3000U,
            },
        },
        .platform_config = {
            .storage_partition_name = ZIGBEE_STORAGE_PARTITION,
            .radio_config = {.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE},
        },
    };
    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(
        ezb_bdb_set_primary_channel_set(PRIMARY_CHANNEL_MASK)));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(
        ezb_app_signal_add_handler(app_signal_handler)));
    ESP_ERROR_CHECK(create_device());
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    ESP_ERROR_CHECK(esp_zigbee_launch_mainloop());
    vTaskDelete(NULL);
}

esp_err_t zigbee_transport_start(soil_policy_t *policy,
                                 soil_state_t *state,
                                 soil_diagnostics_t *diagnostics,
                                 bool ota_mode)
{
    if (!policy || !state || !diagnostics) return ESP_ERR_INVALID_ARG;
    s_policy = policy;
    s_state = state;
    s_diagnostics = diagnostics;
    s_ota_mode = ota_mode;
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "event group allocation failed");
    const BaseType_t ok = xTaskCreate(zigbee_task, "zigbee", 8192, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool zigbee_transport_wait_ready(uint32_t timeout_ms)
{
    if (!s_events) return false;
    const EventBits_t bits = xEventGroupWaitBits(
        s_events, READY_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & READY_BIT) != 0U;
}

static void report_confirm_callback(ezb_af_user_cnf_t *cnf, void *user_ctx)
{
    report_confirm_context_t *ctx = (report_confirm_context_t *)user_ctx;
    if (!s_events || !cnf || !ctx) return;
    xEventGroupSetBits(s_events,
        cnf->status == AF_CONFIRM_SUCCESS ? ctx->success_bit : ctx->failure_bit);
}

static ezb_err_t report_attribute(uint16_t cluster_id,
                                  uint16_t attr_id,
                                  report_confirm_context_t *confirm_ctx)
{
    ezb_zcl_report_attr_cmd_t command = {
        .cmd_ctrl = {
            .fc.direction = EZB_ZCL_CMD_DIRECTION_TO_CLI,
            .dst_addr = {
                .addr_mode = EZB_ADDR_MODE_SHORT,
                .u.short_addr = COORDINATOR_SHORT_ADDRESS,
            },
            .dst_ep = COORDINATOR_ENDPOINT,
            .src_ep = ENDPOINT_ID,
            .cluster_id = cluster_id,
            .cnf_ctx = {
                .cb = report_confirm_callback,
                .user_ctx = confirm_ctx,
            },
        },
        .payload = {.attr_id = attr_id},
    };
    return ezb_zcl_report_attr_cmd_req(&command);
}

static bool wait_for_report(EventBits_t success_bit,
                            EventBits_t failure_bit,
                            uint32_t timeout_ms)
{
    const EventBits_t result = xEventGroupWaitBits(
        s_events, success_bit | failure_bit, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    return (result & success_bit) != 0U && (result & failure_bit) == 0U;
}

esp_err_t zigbee_transport_publish(const soil_state_t *state,
                                   const soil_diagnostics_t *diag)
{
    if (!state || !diag || !s_events) return ESP_ERR_INVALID_ARG;
    const EventBits_t report_bits = REPORT_MOISTURE_CONFIRMED_BIT |
                                     REPORT_MOISTURE_FAILED_BIT |
                                     REPORT_BATTERY_PERCENT_CONFIRMED_BIT |
                                     REPORT_BATTERY_PERCENT_FAILED_BIT |
                                     REPORT_BATTERY_VOLTAGE_CONFIRMED_BIT |
                                     REPORT_BATTERY_VOLTAGE_FAILED_BIT |
                                     REPORT_TELEMETRY_CONFIRMED_BIT |
                                     REPORT_TELEMETRY_FAILED_BIT;
    xEventGroupClearBits(s_events, report_bits);

    const float moisture = state->current_sample_valid ? state->moisture_pct : NAN;
    s_attr_battery_percentage = clamp_u8(state->battery_pct * 2.0f);
    s_attr_battery_voltage = clamp_u8(diag->battery_mv / 100.0f);
    const uint8_t status_flags = state->sensor_fault ? 0x02U
                                 : state->mode == SOIL_MODE_CRITICAL ? 0x01U
                                                                     : 0x00U;
    encode_telemetry(state, diag);

    if (!acquire_zigbee_lock("state publish")) return ESP_ERR_TIMEOUT;
    sync_control_cluster();
    const ezb_zcl_status_t moisture_status = ezb_zcl_set_attr_value(
        ENDPOINT_ID, EZB_ZCL_CLUSTER_ID_ANALOG_INPUT, EZB_ZCL_CLUSTER_SERVER,
        EZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, EZB_ZCL_STD_MANUF_CODE,
        (void *)&moisture, false);
    const ezb_zcl_status_t flags_status = ezb_zcl_set_attr_value(
        ENDPOINT_ID, EZB_ZCL_CLUSTER_ID_ANALOG_INPUT, EZB_ZCL_CLUSTER_SERVER,
        EZB_ZCL_ATTR_ANALOG_INPUT_STATUS_FLAGS_ID, EZB_ZCL_STD_MANUF_CODE,
        (void *)&status_flags, false);
    const ezb_zcl_status_t telemetry_status = ezb_zcl_set_attr_value(
        ENDPOINT_ID, TELEMETRY_CLUSTER_ID, EZB_ZCL_CLUSTER_SERVER,
        TELEMETRY_ATTR_ID, EZB_ZCL_STD_MANUF_CODE, s_telemetry, false);

    ezb_zcl_status_t battery_pct_status = EZB_ZCL_STATUS_SUCCESS;
    ezb_zcl_status_t battery_mv_status = EZB_ZCL_STATUS_SUCCESS;
    if (state->battery_present) {
        battery_pct_status = ezb_zcl_set_attr_value(
            ENDPOINT_ID, EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_CLUSTER_SERVER,
            EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
            EZB_ZCL_STD_MANUF_CODE, &s_attr_battery_percentage, false);
        battery_mv_status = ezb_zcl_set_attr_value(
            ENDPOINT_ID, EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_CLUSTER_SERVER,
            EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
            EZB_ZCL_STD_MANUF_CODE, &s_attr_battery_voltage, false);
    }

    const ezb_err_t moisture_request = report_attribute(
        EZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
        EZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, &s_moisture_confirm);
    const ezb_err_t telemetry_request = report_attribute(
        TELEMETRY_CLUSTER_ID, TELEMETRY_ATTR_ID, &s_telemetry_confirm);
    ezb_err_t battery_pct_request = EZB_ERR_NONE;
    ezb_err_t battery_mv_request = EZB_ERR_NONE;
    if (state->battery_present) {
        battery_pct_request = report_attribute(
            EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
            EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
            &s_battery_percent_confirm);
        battery_mv_request = report_attribute(
            EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
            EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
            &s_battery_voltage_confirm);
    }
    esp_zigbee_lock_release();

    if (moisture_status != EZB_ZCL_STATUS_SUCCESS ||
        flags_status != EZB_ZCL_STATUS_SUCCESS ||
        telemetry_status != EZB_ZCL_STATUS_SUCCESS ||
        moisture_request != EZB_ERR_NONE || telemetry_request != EZB_ERR_NONE) {
        return ESP_FAIL;
    }
    if (!wait_for_report(REPORT_MOISTURE_CONFIRMED_BIT,
                         REPORT_MOISTURE_FAILED_BIT,
                         REPORT_CONFIRM_TIMEOUT_MS) ||
        !wait_for_report(REPORT_TELEMETRY_CONFIRMED_BIT,
                         REPORT_TELEMETRY_FAILED_BIT,
                         REPORT_CONFIRM_TIMEOUT_MS)) {
        return ESP_FAIL;
    }

    if (state->battery_present) {
        if (battery_pct_status == EZB_ZCL_STATUS_SUCCESS &&
            battery_pct_request == EZB_ERR_NONE) {
            (void)wait_for_report(REPORT_BATTERY_PERCENT_CONFIRMED_BIT,
                                  REPORT_BATTERY_PERCENT_FAILED_BIT,
                                  OPTIONAL_REPORT_CONFIRM_TIMEOUT_MS);
        }
        if (battery_mv_status == EZB_ZCL_STATUS_SUCCESS &&
            battery_mv_request == EZB_ERR_NONE) {
            (void)wait_for_report(REPORT_BATTERY_VOLTAGE_CONFIRMED_BIT,
                                  REPORT_BATTERY_VOLTAGE_FAILED_BIT,
                                  OPTIONAL_REPORT_CONFIRM_TIMEOUT_MS);
        }
    }
    return ESP_OK;
}

bool zigbee_transport_wait_config_change(uint32_t timeout_ms)
{
    if (!s_events) return false;
    const EventBits_t bits = xEventGroupWaitBits(
        s_events, CONFIG_CHANGED_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & CONFIG_CHANGED_BIT) != 0U;
}

bool zigbee_transport_take_identify_request(void)
{
    if (!s_events) return false;
    const EventBits_t bits = xEventGroupClearBits(s_events, IDENTIFY_REQUEST_BIT);
    return (bits & IDENTIFY_REQUEST_BIT) != 0U;
}

esp_err_t zigbee_transport_begin_ota_query(void)
{
    if (!s_ota_mode || !s_events) return ESP_ERR_INVALID_STATE;
    if (!acquire_zigbee_lock("OTA image query")) return ESP_ERR_TIMEOUT;
    const esp_err_t err = firmware_update_request_image(ENDPOINT_ID);
    esp_zigbee_lock_release();
    return err;
}
