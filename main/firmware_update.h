#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ezbee/zcl/cluster/ota_upgrade.h"

#define SOIL_OTA_MANUFACTURER_CODE 0xFFF1U
#define SOIL_OTA_IMAGE_TYPE        0x0001U
#define SOIL_OTA_MIN_BATTERY_MV    1250.0f

/*
 * The Zigbee OTA file version is derived from PROJECT_VER (CMakeLists.txt) as
 * 0xMMmmpp so it always tracks the build: the coordinator refuses to offer any
 * image whose version is not newer than the client's advertised one, so a
 * hand-maintained constant was a silent release breaker. The ESP-IDF build
 * defines PROJECT_VER_MAJOR/MINOR/PATCH when PROJECT_VER matches x.y.z; the
 * fallbacks below keep out-of-tree builds compiling and MUST stay in sync with
 * CMakeLists.txt. If the build provides a PROJECT_VER that is not x.y.z, fail
 * loudly instead of silently advertising a possibly-stale version.
 */
#if defined(PROJECT_VER) && !defined(PROJECT_VER_MAJOR)
#error "PROJECT_VER is not x.y.z: fix CMakeLists.txt or the SOIL_OTA_FILE_VERSION fallback in firmware_update.h"
#endif
#ifndef PROJECT_VER_MAJOR
#define PROJECT_VER_MAJOR 1
#define PROJECT_VER_MINOR 0
#define PROJECT_VER_PATCH 2
#endif
#define SOIL_OTA_FILE_VERSION                                                    \
    ((((uint32_t)(PROJECT_VER_MAJOR) & 0xFFU) << 16U) |                         \
     (((uint32_t)(PROJECT_VER_MINOR) & 0xFFU) << 8U) |                          \
     ((uint32_t)(PROJECT_VER_PATCH) & 0xFFU))

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
