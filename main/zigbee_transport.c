#include "zigbee_transport.h"

#include <math.h>
#include <string.h>
#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_zigbee.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "ezbee/af.h"
#include "ezbee/app_signals.h"
#include "ezbee/bdb.h"
#include "ezbee/nwk.h"
#include "ezbee/zcl/zcl_common.h"
#include "ezbee/zcl/zcl_general_cmd.h"
#include "ezbee/zcl/cluster/analog_input_desc.h"
#include "ezbee/zcl/cluster/basic_desc.h"
#include "ezbee/zcl/cluster/custom.h"
#include "ezbee/zcl/cluster/identify_desc.h"
#include "ezbee/zcl/cluster/power_config_desc.h"

#define ENDPOINT_ID 1
#define CUSTOM_DEVICE_ID 0xFFF0
#define TELEMETRY_CLUSTER_ID 0xFC00
#define TELEMETRY_ATTR_ID 0x0000
#define TELEMETRY_SCHEMA_VERSION 1U
#define TELEMETRY_PAYLOAD_LENGTH 21U
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

#define PRIMARY_CHANNEL_MASK 0x07FFF800UL
#define ZIGBEE_STORAGE_PARTITION "zb_storage"
#define COMMISSIONING_RETRY_DELAY_MS 1000U
#define REPORT_CONFIRM_TIMEOUT_MS 3000U
#define OPTIONAL_REPORT_CONFIRM_TIMEOUT_MS 1000U
#define COORDINATOR_SHORT_ADDRESS 0x0000U
#define COORDINATOR_ENDPOINT 1U
#define AF_CONFIRM_SUCCESS 0x00U

static const char *TAG = "zigbee";
static EventGroupHandle_t s_events;
static volatile bool s_retry_pending;
static ezb_bdb_comm_mode_mask_t s_retry_mode;
static uint8_t s_telemetry[TELEMETRY_BUFFER_LENGTH] = {TELEMETRY_PAYLOAD_LENGTH, TELEMETRY_SCHEMA_VERSION};

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

static void encode_telemetry(const soil_state_t *state, const soil_diagnostics_t *diag)
{
    memset(s_telemetry, 0, sizeof(s_telemetry));
    s_telemetry[0] = TELEMETRY_PAYLOAD_LENGTH;
    s_telemetry[1] = TELEMETRY_SCHEMA_VERSION;
    s_telemetry[2] = (uint8_t)state->mode;
    put_u16_le(&s_telemetry[3], (uint16_t)(state->event_flags & UINT16_MAX));
    s_telemetry[5] = state->sensor_fault ? 1U : 0U;
    s_telemetry[6] = clamp_u8(state->confidence_pct);
    put_u16_le(&s_telemetry[7], clamp_u16(diag->raw_mv));
    put_u16_le(&s_telemetry[9], clamp_u16(diag->noise_mv));
    put_u16_le(&s_telemetry[11], (uint16_t)clamp_i16(state->drying_rate_pct_per_hour * 100.0f));
    put_u32_le(&s_telemetry[13], state->sample_interval_seconds);
    put_u32_le(&s_telemetry[17], state->seconds_since_watering);
    s_telemetry[21] = state->battery_present ? 1U : 0U;
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

static ezb_err_t start_commissioning(ezb_bdb_comm_mode_mask_t mode, const char *reason)
{
    const ezb_err_t err = ezb_bdb_start_top_level_commissioning(mode);
    if (err == EZB_ERR_NONE) {
        ESP_LOGI(TAG, "started BDB commissioning mode=0x%02x (%s)", (unsigned)mode, reason);
    } else {
        ESP_LOGE(TAG, "failed to start BDB commissioning mode=0x%02x (%s), error=0x%04x",
                 (unsigned)mode, reason, (unsigned)err);
    }
    return err;
}

static void commissioning_retry_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(COMMISSIONING_RETRY_DELAY_MS));

    const ezb_bdb_comm_mode_mask_t mode = s_retry_mode;
    esp_zigbee_lock_acquire(portMAX_DELAY);
    const ezb_err_t err = start_commissioning(mode, "scheduled retry");
    esp_zigbee_lock_release();

    s_retry_pending = false;
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "commissioning retry request was rejected; waiting for the next stack signal");
    }
    vTaskDelete(NULL);
}

static void schedule_commissioning_retry(ezb_bdb_comm_mode_mask_t mode)
{
    if (s_retry_pending) {
        return;
    }

    s_retry_mode = mode;
    s_retry_pending = true;
    if (xTaskCreate(commissioning_retry_task, "zb_retry", 3072, NULL, 4, NULL) != pdPASS) {
        s_retry_pending = false;
        ESP_LOGE(TAG, "failed to create Zigbee commissioning retry task");
    } else {
        ESP_LOGI(TAG, "scheduled BDB commissioning retry mode=0x%02x in %u ms",
                 (unsigned)mode, COMMISSIONING_RETRY_DELAY_MS);
    }
}

static bool app_signal_handler(const ezb_app_signal_t *signal)
{
    const ezb_app_signal_type_t type = ezb_app_signal_get_type(signal);
    const char *signal_name = ezb_app_signal_to_string(type);

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee signal: %s (0x%02x)", signal_name, (unsigned)type);
        (void)start_commissioning(EZB_BDB_MODE_INITIALIZATION, "stack startup");
        break;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        const ezb_bdb_comm_status_t status = *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        const bool factory_new = ezb_bdb_is_factory_new();
        ESP_LOGI(TAG, "Zigbee signal: %s (0x%02x), status=0x%02x, factory_new=%s",
                 signal_name, (unsigned)type, (unsigned)status, factory_new ? "yes" : "no");

        if (status == EZB_BDB_STATUS_SUCCESS) {
            if (factory_new) {
                const esp_err_t led_err = board_pairing_indicator_start();
                if (led_err != ESP_OK) {
                    ESP_LOGW(TAG, "failed to start pairing LED indicator: %s", esp_err_to_name(led_err));
                }
                (void)start_commissioning(EZB_BDB_MODE_NETWORK_STEERING, "factory-new device");
            } else {
                ESP_LOGI(TAG, "restored existing Zigbee network state");
                xEventGroupSetBits(s_events, READY_BIT);
            }
        } else {
            ESP_LOGW(TAG, "Zigbee initialization failed with status=0x%02x", (unsigned)status);
            schedule_commissioning_retry(EZB_BDB_MODE_INITIALIZATION);
        }
        break;
    }

    case EZB_BDB_SIGNAL_STEERING: {
        const ezb_bdb_comm_status_t status = *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t extended_pan_id;
            ezb_nwk_get_extended_panid(&extended_pan_id);
            ESP_LOGI(TAG,
                     "joined Zigbee network: PAN=0x%04x EXT=0x%016llx channel=%u short=0x%04x",
                     ezb_nwk_get_panid(), (unsigned long long)extended_pan_id.u64,
                     (unsigned)ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            board_pairing_indicator_success();
            xEventGroupSetBits(s_events, READY_BIT);
        } else {
            ESP_LOGW(TAG, "network steering failed with status=0x%02x", (unsigned)status);
            schedule_commissioning_retry(EZB_BDB_MODE_NETWORK_STEERING);
        }
        break;
    }

    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        const uint8_t duration = *(const uint8_t *)ezb_app_signal_get_params(signal);
        ESP_LOGI(TAG, "permit-join status: duration=%u seconds", (unsigned)duration);
        break;
    }

    default:
        ESP_LOGI(TAG, "Zigbee signal: %s (0x%02x)", signal_name, (unsigned)type);
        break;
    }
    return true;
}

static void telemetry_cluster_init(uint8_t ep_id)
{
    (void)ep_id;
}

static esp_err_t create_device(void)
{
    ezb_af_device_desc_t device = ezb_af_create_device_desc();
    ESP_RETURN_ON_FALSE(device, ESP_ERR_NO_MEM, TAG, "device descriptor allocation failed");

    ezb_af_ep_config_t ep_cfg = {
        .ep_id = ENDPOINT_ID,
        .app_profile_id = EZB_AF_HA_PROFILE_ID,
        .app_device_id = CUSTOM_DEVICE_ID,
        .app_device_version = 2,
    };
    ezb_af_ep_desc_t endpoint = ezb_af_create_endpoint_desc(&ep_cfg);
    ESP_RETURN_ON_FALSE(endpoint, ESP_ERR_NO_MEM, TAG, "endpoint allocation failed");

    ezb_zcl_basic_cluster_server_config_t basic_cfg = {
        .zcl_version = EZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = EZB_ZCL_BASIC_POWER_SOURCE_BATTERY,
    };
    ezb_zcl_cluster_desc_t basic = ezb_zcl_basic_create_cluster_desc(&basic_cfg, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)"\x0b""longhornx10");
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)"\x0d""Soil Sentinel");

    ezb_zcl_identify_cluster_server_config_t identify_cfg = {.identify_time = 0};
    ezb_zcl_cluster_desc_t identify = ezb_zcl_identify_create_cluster_desc(&identify_cfg, EZB_ZCL_CLUSTER_SERVER);

    ezb_zcl_analog_input_cluster_server_config_t moisture_cfg = {
        .out_of_service = false,
        .present_value = 0.0f,
        .status_flags = 0,
    };
    ezb_zcl_cluster_desc_t moisture = ezb_zcl_analog_input_create_cluster_desc(&moisture_cfg, EZB_ZCL_CLUSTER_SERVER);
    float min_value = 0.0f, max_value = 100.0f, resolution = 0.1f;
    uint16_t engineering_units = 98; /* percent */
    ezb_zcl_analog_input_cluster_desc_add_attr(moisture, EZB_ZCL_ATTR_ANALOG_INPUT_MIN_PRESENT_VALUE_ID, &min_value);
    ezb_zcl_analog_input_cluster_desc_add_attr(moisture, EZB_ZCL_ATTR_ANALOG_INPUT_MAX_PRESENT_VALUE_ID, &max_value);
    ezb_zcl_analog_input_cluster_desc_add_attr(moisture, EZB_ZCL_ATTR_ANALOG_INPUT_RESOLUTION_ID, &resolution);
    ezb_zcl_analog_input_cluster_desc_add_attr(moisture, EZB_ZCL_ATTR_ANALOG_INPUT_ENGINEERING_UNITS_ID, &engineering_units);
    ezb_zcl_analog_input_cluster_desc_add_attr(moisture, EZB_ZCL_ATTR_ANALOG_INPUT_DESCRIPTION_ID, (void *)"\x0d""Soil moisture");

    ezb_zcl_cluster_desc_t power = ezb_zcl_power_config_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_SERVER);
    uint8_t battery_voltage = EZB_ZCL_VALUE_UINT8_FF;
    uint8_t battery_percentage = EZB_ZCL_VALUE_UINT8_FF;
    uint8_t battery_size = EZB_ZCL_POWER_CONFIG_BATTERY_SIZE_AA;
    uint8_t battery_quantity = 1U;
    uint8_t battery_rated_voltage = 15U; /* 1.5 V in 100 mV units */
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_zcl_power_config_cluster_desc_add_attr(
        power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID, &battery_voltage)));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_zcl_power_config_cluster_desc_add_attr(
        power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, &battery_percentage)));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_zcl_power_config_cluster_desc_add_attr(
        power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_SIZE_ID, &battery_size)));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_zcl_power_config_cluster_desc_add_attr(
        power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_QUANTITY_ID, &battery_quantity)));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_zcl_power_config_cluster_desc_add_attr(
        power, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_RATED_VOLTAGE_ID, &battery_rated_voltage)));

    ezb_zcl_custom_cluster_config_t telemetry_cfg = {
        .cluster_id = TELEMETRY_CLUSTER_ID,
        .init_func = telemetry_cluster_init,
        .deinit_func = NULL,
    };
    ezb_zcl_cluster_desc_t telemetry = ezb_zcl_custom_create_cluster_desc(&telemetry_cfg, EZB_ZCL_CLUSTER_SERVER);
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_zcl_custom_cluster_desc_add_attr(
        telemetry,
        TELEMETRY_ATTR_ID,
        EZB_ZCL_ATTR_TYPE_OCTSTR,
        EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING,
        s_telemetry)));

    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, basic));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, identify));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, moisture));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, power));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, telemetry));
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(device, endpoint));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(device));
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
                .keep_alive = 3000,
            },
        },
        .platform_config = {
            .storage_partition_name = ZIGBEE_STORAGE_PARTITION,
            .radio_config = {
                .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,
            },
        },
    };
    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_bdb_set_primary_channel_set(PRIMARY_CHANNEL_MASK)));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_app_signal_add_handler(app_signal_handler)));
    ESP_ERROR_CHECK(create_device());
    ESP_LOGI(TAG, "starting Zigbee stack; primary channel mask=0x%08lx", (unsigned long)PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    ESP_ERROR_CHECK(esp_zigbee_launch_mainloop());
    vTaskDelete(NULL);
}

esp_err_t zigbee_transport_start(void)
{
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "event group allocation failed");
    const BaseType_t ok = xTaskCreate(zigbee_task, "zigbee", 6144, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool zigbee_transport_wait_ready(uint32_t timeout_ms)
{
    if (!s_events) return false;
    const EventBits_t bits = xEventGroupWaitBits(s_events, READY_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & READY_BIT) != 0;
}

static void report_confirm_callback(ezb_af_user_cnf_t *cnf, void *user_ctx)
{
    report_confirm_context_t *ctx = (report_confirm_context_t *)user_ctx;
    if (!s_events || !cnf || !ctx) {
        return;
    }

    if (cnf->status == AF_CONFIRM_SUCCESS) {
        ESP_LOGI(TAG,
                 "%s report confirmed: dst=0x%04x ep=%u cluster=0x%04x tsn=%u",
                 ctx->name, cnf->dst_addr.u.short_addr, (unsigned)cnf->dst_ep,
                 cnf->cluster_id, (unsigned)cnf->tsn);
        xEventGroupSetBits(s_events, ctx->success_bit);
    } else {
        ESP_LOGW(TAG,
                 "%s report transmission failed: status=0x%02x dst=0x%04x ep=%u cluster=0x%04x",
                 ctx->name, (unsigned)cnf->status, cnf->dst_addr.u.short_addr,
                 (unsigned)cnf->dst_ep, cnf->cluster_id);
        xEventGroupSetBits(s_events, ctx->failure_bit);
    }
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
                            uint32_t timeout_ms,
                            const char *name)
{
    const EventBits_t result = xEventGroupWaitBits(
        s_events, success_bit | failure_bit, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    const bool confirmed = (result & success_bit) != 0 && (result & failure_bit) == 0;
    if (!confirmed) {
        ESP_LOGW(TAG, "%s report was not confirmed within %u ms", name, timeout_ms);
    }
    return confirmed;
}

esp_err_t zigbee_transport_publish(const soil_state_t *state, const soil_diagnostics_t *diag)
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

    const float moisture = state->moisture_pct;
    const uint8_t battery_half_pct = clamp_u8(state->battery_pct * 2.0f);
    const uint8_t battery_voltage_tenths = clamp_u8(diag->battery_mv / 100.0f);
    const uint8_t status_flags = state->sensor_fault ? 0x02U
                                : state->mode == SOIL_MODE_CRITICAL ? 0x01U
                                                                    : 0x00U;
    encode_telemetry(state, diag);

    esp_zigbee_lock_acquire(portMAX_DELAY);
    const ezb_zcl_status_t moisture_status = ezb_zcl_set_attr_value(
        ENDPOINT_ID, EZB_ZCL_CLUSTER_ID_ANALOG_INPUT, EZB_ZCL_CLUSTER_SERVER,
        EZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, EZB_ZCL_STD_MANUF_CODE, (void *)&moisture, false);
    const ezb_zcl_status_t flags_status = ezb_zcl_set_attr_value(
        ENDPOINT_ID, EZB_ZCL_CLUSTER_ID_ANALOG_INPUT, EZB_ZCL_CLUSTER_SERVER,
        EZB_ZCL_ATTR_ANALOG_INPUT_STATUS_FLAGS_ID, EZB_ZCL_STD_MANUF_CODE, (void *)&status_flags, false);
    const ezb_zcl_status_t telemetry_status = ezb_zcl_set_attr_value(
        ENDPOINT_ID, TELEMETRY_CLUSTER_ID, EZB_ZCL_CLUSTER_SERVER,
        TELEMETRY_ATTR_ID, EZB_ZCL_STD_MANUF_CODE, s_telemetry, false);

    ezb_zcl_status_t battery_percent_status = EZB_ZCL_STATUS_SUCCESS;
    ezb_zcl_status_t battery_voltage_status = EZB_ZCL_STATUS_SUCCESS;
    if (state->battery_present) {
        battery_percent_status = ezb_zcl_set_attr_value(
            ENDPOINT_ID, EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_CLUSTER_SERVER,
            EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
            EZB_ZCL_STD_MANUF_CODE, (void *)&battery_half_pct, false);
        battery_voltage_status = ezb_zcl_set_attr_value(
            ENDPOINT_ID, EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_CLUSTER_SERVER,
            EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
            EZB_ZCL_STD_MANUF_CODE, (void *)&battery_voltage_tenths, false);
    }

    const ezb_err_t moisture_report_status = report_attribute(
        EZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
        EZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID,
        &s_moisture_confirm);
    const ezb_err_t telemetry_report_status = report_attribute(
        TELEMETRY_CLUSTER_ID,
        TELEMETRY_ATTR_ID,
        &s_telemetry_confirm);

    ezb_err_t battery_percent_report_status = EZB_ERR_NONE;
    ezb_err_t battery_voltage_report_status = EZB_ERR_NONE;
    if (state->battery_present) {
        battery_percent_report_status = report_attribute(
            EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
            EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
            &s_battery_percent_confirm);
        battery_voltage_report_status = report_attribute(
            EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
            EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
            &s_battery_voltage_confirm);
    }
    esp_zigbee_lock_release();

    ESP_LOGI(TAG,
             "queued coordinator state moisture=%.1f battery=%s%.1f raw=%.0f noise=%.1f mode=%s "
             "attrs=[0x%02x,0x%02x,0x%02x,0x%02x,0x%02x] requests=[0x%04x,0x%04x,0x%04x,0x%04x]",
             state->moisture_pct, state->battery_present ? "" : "n/a ", state->battery_pct,
             diag->raw_mv, diag->noise_mv, soil_mode_name(state->mode),
             (unsigned)moisture_status, (unsigned)flags_status, (unsigned)telemetry_status,
             (unsigned)battery_percent_status, (unsigned)battery_voltage_status,
             (unsigned)moisture_report_status, (unsigned)telemetry_report_status,
             (unsigned)battery_percent_report_status, (unsigned)battery_voltage_report_status);

    if (moisture_status != EZB_ZCL_STATUS_SUCCESS ||
        flags_status != EZB_ZCL_STATUS_SUCCESS ||
        telemetry_status != EZB_ZCL_STATUS_SUCCESS ||
        moisture_report_status != EZB_ERR_NONE ||
        telemetry_report_status != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "required state report was rejected before transmission");
        return ESP_FAIL;
    }

    const bool moisture_confirmed = wait_for_report(
        REPORT_MOISTURE_CONFIRMED_BIT,
        REPORT_MOISTURE_FAILED_BIT,
        REPORT_CONFIRM_TIMEOUT_MS,
        "moisture");
    const bool telemetry_confirmed = wait_for_report(
        REPORT_TELEMETRY_CONFIRMED_BIT,
        REPORT_TELEMETRY_FAILED_BIT,
        REPORT_CONFIRM_TIMEOUT_MS,
        "telemetry");

    if (!moisture_confirmed || !telemetry_confirmed) {
        return ESP_FAIL;
    }

    if (state->battery_present) {
        if (battery_percent_status == EZB_ZCL_STATUS_SUCCESS &&
            battery_percent_report_status == EZB_ERR_NONE) {
            (void)wait_for_report(
                REPORT_BATTERY_PERCENT_CONFIRMED_BIT,
                REPORT_BATTERY_PERCENT_FAILED_BIT,
                OPTIONAL_REPORT_CONFIRM_TIMEOUT_MS,
                "battery percentage");
        } else {
            ESP_LOGW(TAG, "battery percentage attribute/report request unavailable");
        }

        if (battery_voltage_status == EZB_ZCL_STATUS_SUCCESS &&
            battery_voltage_report_status == EZB_ERR_NONE) {
            (void)wait_for_report(
                REPORT_BATTERY_VOLTAGE_CONFIRMED_BIT,
                REPORT_BATTERY_VOLTAGE_FAILED_BIT,
                OPTIONAL_REPORT_CONFIRM_TIMEOUT_MS,
                "battery voltage");
        } else {
            ESP_LOGW(TAG, "battery voltage attribute/report request unavailable");
        }
    }

    return ESP_OK;
}
