#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ezbee/zcl/cluster/ota_upgrade.h"

#define SOIL_OTA_MANUFACTURER_CODE 0xFFF1U
#define SOIL_OTA_IMAGE_TYPE        0x0001U
#define SOIL_OTA_FILE_VERSION      0x00010000UL
#define SOIL_OTA_MIN_BATTERY_MV    1250.0f

typedef enum {
    SOIL_OTA_STATE_IDLE = 0,
    SOIL_OTA_STATE_ARMED,
    SOIL_OTA_STATE_QUERYING,
    SOIL_OTA_STATE_DOWNLOADING,
    SOIL_OTA_STATE_APPLYING,
    SOIL_OTA_STATE_COMPLETE,
    SOIL_OTA_STATE_FAILED,
    SOIL_OTA_STATE_REFUSED,
} soil_ota_state_t;

typedef enum {
    SOIL_OTA_RESULT_NONE = 0,
    SOIL_OTA_RESULT_SUCCESS,
    SOIL_OTA_RESULT_NO_IMAGE,
    SOIL_OTA_RESULT_LOW_BATTERY,
    SOIL_OTA_RESULT_DOWNLOAD_ERROR,
    SOIL_OTA_RESULT_VALIDATION_ERROR,
    SOIL_OTA_RESULT_ROLLED_BACK,
    SOIL_OTA_RESULT_TIMEOUT,
} soil_ota_result_t;

esp_err_t firmware_update_init(void);
bool firmware_update_needs_boot_validation(void);
esp_err_t firmware_update_mark_running_valid(void);
esp_err_t firmware_update_request_image(uint8_t endpoint_id);
void firmware_update_arm(void);
void firmware_update_refuse_low_battery(void);
void firmware_update_timeout(void);
void firmware_update_handle_progress(ezb_zcl_ota_upgrade_client_progress_message_t *message);
void firmware_update_handle_query_response(ezb_zcl_ota_upgrade_query_next_image_rsp_message_t *message);
soil_ota_state_t firmware_update_state(void);
soil_ota_result_t firmware_update_last_result(void);
uint8_t firmware_update_progress_percent(void);
uint8_t firmware_update_active_slot(void);
uint8_t firmware_update_rollback_state(void);
uint32_t firmware_update_file_version(void);
const char *firmware_update_build_id(void);
