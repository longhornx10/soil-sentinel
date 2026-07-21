#include "zigbee_transport.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_zigbee.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "ezbee/af.h"
#include "ezbee/bdb.h"
#include "ezbee/nwk.h"
#include "ezbee/zcl/zcl_common.h"
#include "ezbee/zcl/zcl_general_cmd.h"
#include "ezbee/zcl/cluster/analog_input_desc.h"
#include "ezbee/zcl/cluster/basic_desc.h"
#include "ezbee/zcl/cluster/identify_desc.h"
#include "ezbee/zcl/cluster/power_config_desc.h"

#define ENDPOINT_ID 1
#define CUSTOM_DEVICE_ID 0xFFF0
#define READY_BIT BIT0
#define PRIMARY_CHANNEL_MASK 0x07FFF800UL
#define ZIGBEE_STORAGE_PARTITION "zb_storage"

static const char *TAG = "zigbee";
static EventGroupHandle_t s_events;

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

static bool app_signal_handler(const ezb_app_signal_t *signal)
{
    const ezb_app_signal_type_t type = ezb_app_signal_get_type(signal);
    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        const ezb_bdb_comm_status_t status = *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            if (ezb_bdb_is_factory_new()) {
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
            } else {
                xEventGroupSetBits(s_events, READY_BIT);
            }
        }
        break;
    }
    case EZB_BDB_SIGNAL_STEERING: {
        const ezb_bdb_comm_status_t status = *(const ezb_bdb_comm_status_t *)ezb_app_signal_get_params(signal);
        if (status == EZB_BDB_STATUS_SUCCESS) xEventGroupSetBits(s_events, READY_BIT);
        break;
    }
    default:
        break;
    }
    return true;
}

static esp_err_t create_device(void)
{
    ezb_af_device_desc_t device = ezb_af_create_device_desc();
    ESP_RETURN_ON_FALSE(device, ESP_ERR_NO_MEM, TAG, "device descriptor allocation failed");

    ezb_af_ep_config_t ep_cfg = {
        .ep_id = ENDPOINT_ID,
        .app_profile_id = EZB_AF_HA_PROFILE_ID,
        .app_device_id = CUSTOM_DEVICE_ID,
        .app_device_version = 1,
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

    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, basic));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, identify));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, moisture));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(endpoint, power));
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(device, endpoint));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(device));
    return ESP_OK;
}

static void zigbee_task(void *arg)
{
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
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    ESP_ERROR_CHECK(esp_zigbee_launch_mainloop());
    vTaskDelete(NULL);
}

esp_err_t zigbee_transport_start(void)
{
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "event group allocation failed");
    BaseType_t ok = xTaskCreate(zigbee_task, "zigbee", 6144, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool zigbee_transport_wait_ready(uint32_t timeout_ms)
{
    if (!s_events) return false;
    EventBits_t bits = xEventGroupWaitBits(s_events, READY_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & READY_BIT) != 0;
}

static ezb_err_t report_attribute(uint16_t cluster_id, uint16_t attr_id)
{
    ezb_zcl_report_attr_cmd_t command = {
        .cmd_ctrl = {
            .fc.direction = EZB_ZCL_CMD_DIRECTION_TO_CLI,
            .dst_addr.addr_mode = EZB_ADDR_MODE_NONE,
            .src_ep = ENDPOINT_ID,
            .cluster_id = cluster_id,
        },
        .payload = {.attr_id = attr_id},
    };
    return ezb_zcl_report_attr_cmd_req(&command);
}

esp_err_t zigbee_transport_publish(const soil_state_t *state, const soil_diagnostics_t *diag)
{
    if (!state || !diag) return ESP_ERR_INVALID_ARG;
    float moisture = state->moisture_pct;
    uint8_t battery_half_pct = (uint8_t)(state->battery_pct * 2.0f + 0.5f);
    uint8_t status_flags = state->sensor_fault ? 0x02 : (state->mode == SOIL_MODE_CRITICAL ? 0x01 : 0x00);

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_status_t moisture_status = ezb_zcl_set_attr_value(
        ENDPOINT_ID, EZB_ZCL_CLUSTER_ID_ANALOG_INPUT, EZB_ZCL_CLUSTER_SERVER,
        EZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, EZB_ZCL_STD_MANUF_CODE, &moisture, false);
    ezb_zcl_status_t flags_status = ezb_zcl_set_attr_value(
        ENDPOINT_ID, EZB_ZCL_CLUSTER_ID_ANALOG_INPUT, EZB_ZCL_CLUSTER_SERVER,
        EZB_ZCL_ATTR_ANALOG_INPUT_STATUS_FLAGS_ID, EZB_ZCL_STD_MANUF_CODE, &status_flags, false);
    ezb_zcl_status_t battery_status = ezb_zcl_set_attr_value(
        ENDPOINT_ID, EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_CLUSTER_SERVER,
        EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, EZB_ZCL_STD_MANUF_CODE, &battery_half_pct, false);
    ezb_err_t report_status = report_attribute(EZB_ZCL_CLUSTER_ID_ANALOG_INPUT, EZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID);
    (void)report_attribute(EZB_ZCL_CLUSTER_ID_POWER_CONFIG, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID);
    esp_zigbee_lock_release();

    ESP_LOGI(TAG, "publish moisture=%.1f battery=%.1f raw=%.0f noise=%.1f mode=%s",
             state->moisture_pct, state->battery_pct, diag->raw_mv, diag->noise_mv, soil_mode_name(state->mode));
    return (moisture_status == EZB_ZCL_STATUS_SUCCESS && flags_status == EZB_ZCL_STATUS_SUCCESS &&
            battery_status == EZB_ZCL_STATUS_SUCCESS && report_status == EZB_ERR_NONE) ? ESP_OK : ESP_FAIL;
}
