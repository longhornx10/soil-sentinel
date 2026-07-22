#include "firmware_update.h"

#include <inttypes.h>
#include <string.h>
#include "board.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_zigbee.h"
#include "nvs.h"
#include "ota_file_parser.h"

#define OTA_NS "soil_ota"
#define KEY_LAST_RESULT "last_result"
#define KEY_PENDING_SLOT "pending_slot"
#define COORDINATOR_SHORT_ADDRESS 0x0000U
#define COORDINATOR_ENDPOINT 1U

static const char *TAG = "soil-ota";
static const esp_partition_t *s_ota_partition;
static esp_zb_ota_file_parser_t *s_parser;
static esp_ota_handle_t s_ota_handle;
static soil_ota_state_t s_state = SOIL_OTA_STATE_IDLE;
static soil_ota_result_t s_last_result = SOIL_OTA_RESULT_NONE;
static uint8_t s_progress;
static bool s_needs_boot_validation;

static void save_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    if (nvs_open(OTA_NS, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u8(handle, key, value) == ESP_OK) (void)nvs_commit(handle);
    nvs_close(handle);
}

static bool load_u8(const char *key, uint8_t *value)
{
    nvs_handle_t handle;
    if (!value || nvs_open(OTA_NS, NVS_READONLY, &handle) != ESP_OK) return false;
    const esp_err_t err = nvs_get_u8(handle, key, value);
    nvs_close(handle);
    return err == ESP_OK;
}

static void set_result(soil_ota_result_t result)
{
    s_last_result = result;
    save_u8(KEY_LAST_RESULT, (uint8_t)result);
}

static void cleanup_transfer(bool abort_handle)
{
    if (abort_handle && s_ota_handle) (void)esp_ota_abort(s_ota_handle);
    s_ota_handle = 0;
    s_ota_partition = NULL;
    esp_zb_free_ota_file_parser(s_parser);
    s_parser = NULL;
}

uint8_t firmware_update_active_slot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return 0xFFU;
    if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) return 0U;
    if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) return 1U;
    return 0xFEU;
}

esp_err_t firmware_update_init(void)
{
    uint8_t saved = SOIL_OTA_RESULT_NONE;
    if (load_u8(KEY_LAST_RESULT, &saved)) s_last_result = (soil_ota_result_t)saved;

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return ESP_ERR_NOT_FOUND;

    esp_ota_img_states_t image_state;
    const esp_err_t state_err = esp_ota_get_state_partition(running, &image_state);
    if (state_err == ESP_OK && image_state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_needs_boot_validation = true;
        return ESP_OK;
    }

    uint8_t pending_slot = 0xFFU;
    if (load_u8(KEY_PENDING_SLOT, &pending_slot) && pending_slot != 0xFFU) {
        if (pending_slot != firmware_update_active_slot()) {
            set_result(SOIL_OTA_RESULT_ROLLED_BACK);
        }
        save_u8(KEY_PENDING_SLOT, 0xFFU);
    }
    return ESP_OK;
}

bool firmware_update_needs_boot_validation(void)
{
    return s_needs_boot_validation;
}

esp_err_t firmware_update_mark_running_valid(void)
{
    if (!s_needs_boot_validation) return ESP_OK;
    ESP_RETURN_ON_ERROR(esp_ota_mark_app_valid_cancel_rollback(), TAG,
                        "failed to confirm OTA image");
    s_needs_boot_validation = false;
    s_state = SOIL_OTA_STATE_COMPLETE;
    set_result(SOIL_OTA_RESULT_SUCCESS);
    save_u8(KEY_PENDING_SLOT, 0xFFU);
    return ESP_OK;
}

void firmware_update_arm(void)
{
    s_state = SOIL_OTA_STATE_ARMED;
    s_progress = 0U;
}

void firmware_update_refuse_low_battery(void)
{
    s_state = SOIL_OTA_STATE_REFUSED;
    set_result(SOIL_OTA_RESULT_LOW_BATTERY);
}

void firmware_update_timeout(void)
{
    if (s_state == SOIL_OTA_STATE_DOWNLOADING || s_state == SOIL_OTA_STATE_APPLYING) {
        cleanup_transfer(true);
    }
    s_state = SOIL_OTA_STATE_FAILED;
    set_result(SOIL_OTA_RESULT_TIMEOUT);
}

esp_err_t firmware_update_request_image(uint8_t endpoint_id)
{
    ezb_zcl_ota_upgrade_query_next_image_req_cmd_t query = {
        .cmd_ctrl = {
            .dst_addr = {
                .addr_mode = EZB_ADDR_MODE_SHORT,
                .u.short_addr = COORDINATOR_SHORT_ADDRESS,
            },
            .dst_ep = COORDINATOR_ENDPOINT,
            .src_ep = endpoint_id,
        },
        .payload = {
            .manuf_code = SOIL_OTA_MANUFACTURER_CODE,
            .image_type = SOIL_OTA_IMAGE_TYPE,
            .file_version = SOIL_OTA_FILE_VERSION,
        },
    };
    s_state = SOIL_OTA_STATE_QUERYING;
    const ezb_err_t err = ezb_zcl_ota_upgrade_query_next_image_cmd_req(&query);
    return err == EZB_ERR_NONE ? ESP_OK : ESP_FAIL;
}

void firmware_update_handle_query_response(ezb_zcl_ota_upgrade_query_next_image_rsp_message_t *message)
{
    if (!message) return;
    if (message->in.image.status == EZB_ZCL_OTA_UPGRADE_STATUS_CODE_SUCCESS) {
        ESP_LOGI(TAG, "OTA image offered: version=0x%08" PRIx32 " size=%" PRIu32,
                 message->in.image.file_version, message->in.image.size);
        message->out.result = EZB_ZCL_STATUS_SUCCESS;
    } else {
        ESP_LOGI(TAG, "no OTA image available (status=0x%02x)",
                 message->in.image.status);
        s_state = SOIL_OTA_STATE_IDLE;
        set_result(SOIL_OTA_RESULT_NO_IMAGE);
        message->out.result = EZB_ZCL_STATUS_SUCCESS;
    }
}

void firmware_update_handle_progress(ezb_zcl_ota_upgrade_client_progress_message_t *message)
{
    if (!message) return;
    esp_err_t ret = ESP_OK;

    switch (message->in.progress) {
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_START:
        cleanup_transfer(true);
        s_ota_partition = esp_ota_get_next_update_partition(NULL);
        ESP_GOTO_ON_FALSE(s_ota_partition, ESP_ERR_NOT_FOUND, exit, TAG,
                          "no OTA partition available");
        s_parser = esp_zb_create_ota_file_parser(message->in.start.image_size);
        ESP_GOTO_ON_FALSE(s_parser, ESP_ERR_NO_MEM, exit, TAG,
                          "failed to allocate OTA parser");
        ESP_GOTO_ON_ERROR(esp_ota_begin(s_ota_partition, 0, &s_ota_handle), exit,
                          TAG, "failed to begin OTA");
        s_state = SOIL_OTA_STATE_DOWNLOADING;
        s_progress = 0U;
        break;

    case EZB_ZCL_OTA_UPGRADE_PROGRESS_RECEIVING:
        ESP_GOTO_ON_FALSE(s_parser && s_ota_handle && s_ota_partition,
                          ESP_ERR_INVALID_STATE, exit, TAG,
                          "OTA block received without active transfer");
        esp_zb_ota_file_parser_setup(s_parser,
                                     message->in.receiving.block_size,
                                     message->in.receiving.block);
        do {
            ret = esp_zb_ota_file_parser_process(s_parser);
            if (esp_zb_ota_file_parser_is_element_value(s_parser) &&
                s_parser->element.type == UPGRADE_IMAGE) {
                ESP_GOTO_ON_FALSE(s_parser->element.total <= s_ota_partition->size,
                                  ESP_ERR_INVALID_SIZE, exit, TAG,
                                  "OTA image exceeds partition");
                ESP_GOTO_ON_ERROR(esp_ota_write(s_ota_handle,
                                                s_parser->element.val,
                                                s_parser->element.length),
                                  exit, TAG, "failed to write OTA image");
            }
        } while (ret == ESP_ERR_NOT_FINISHED);
        ret = ESP_OK;
        if (s_parser->total_image_size > 0U) {
            const uint32_t received = message->in.receiving.file_offset +
                                      message->in.receiving.block_size;
            const uint32_t pct = received * 100U / s_parser->total_image_size;
            s_progress = pct > 100U ? 100U : (uint8_t)pct;
            board_indicator_ota_progress(s_progress);
        }
        break;

    case EZB_ZCL_OTA_UPGRADE_PROGRESS_CHECK:
        ESP_GOTO_ON_FALSE(s_parser, ESP_ERR_INVALID_STATE, exit, TAG,
                          "OTA check without parser");
        ESP_GOTO_ON_ERROR(esp_zb_ota_file_parser_check(s_parser), exit, TAG,
                          "Zigbee OTA container incomplete");
        break;

    case EZB_ZCL_OTA_UPGRADE_PROGRESS_APPLY:
        s_state = SOIL_OTA_STATE_APPLYING;
        ESP_GOTO_ON_ERROR(esp_ota_end(s_ota_handle), exit, TAG,
                          "failed to validate application image");
        s_ota_handle = 0;
        ESP_GOTO_ON_ERROR(esp_ota_set_boot_partition(s_ota_partition), exit, TAG,
                          "failed to select OTA boot partition");
        save_u8(KEY_PENDING_SLOT,
                s_ota_partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? 0U : 1U);
        break;

    case EZB_ZCL_OTA_UPGRADE_PROGRESS_FINISH:
        s_progress = 100U;
        s_state = SOIL_OTA_STATE_COMPLETE;
        board_indicator_ota_success();
        /* Success is persisted only after the new application validates its first
         * boot. Until then the bootloader rollback state is still authoritative. */
        cleanup_transfer(false);
        esp_restart();
        break;

    case EZB_ZCL_OTA_UPGRADE_PROGRESS_ABORT:
        cleanup_transfer(true);
        s_state = SOIL_OTA_STATE_FAILED;
        set_result(SOIL_OTA_RESULT_DOWNLOAD_ERROR);
        board_indicator_ota_failure();
        break;

    default:
        ESP_LOGW(TAG, "unknown OTA progress state %d", message->in.progress);
        break;
    }

exit:
    if (ret != ESP_OK) {
        cleanup_transfer(true);
        s_state = SOIL_OTA_STATE_FAILED;
        set_result(message->in.progress == EZB_ZCL_OTA_UPGRADE_PROGRESS_CHECK ||
                           message->in.progress == EZB_ZCL_OTA_UPGRADE_PROGRESS_APPLY
                       ? SOIL_OTA_RESULT_VALIDATION_ERROR
                       : SOIL_OTA_RESULT_DOWNLOAD_ERROR);
        board_indicator_ota_failure();
    }
    message->out.result = ret == ESP_OK ? EZB_ZCL_STATUS_SUCCESS
                                        : EZB_ZCL_STATUS_ABORT;
}

soil_ota_state_t firmware_update_state(void) { return s_state; }
soil_ota_result_t firmware_update_last_result(void) { return s_last_result; }
uint8_t firmware_update_progress_percent(void) { return s_progress; }
uint8_t firmware_update_rollback_state(void) { return s_needs_boot_validation ? 1U : 0U; }
uint32_t firmware_update_file_version(void) { return SOIL_OTA_FILE_VERSION; }

const char *firmware_update_build_id(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->version : "unknown";
}
