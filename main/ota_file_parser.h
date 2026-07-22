/* SPDX-License-Identifier: CC0-1.0
 * Derived from Espressif's ESP Zigbee SDK OTA client example.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ezbee/zcl/cluster/ota_upgrade.h"

#define OTA_ELEMENT_HEADER_LEN (sizeof(uint16_t) + sizeof(uint32_t))

typedef enum {
    UPGRADE_IMAGE = 0x0000,
} esp_ota_element_tag_id_t;

typedef struct esp_zb_ota_file_parser_s {
    uint32_t offset;
    uint32_t total_offset;
    uint32_t total_image_size;
    uint16_t in_length;
    uint8_t *in;
    struct {
        ezb_zcl_ota_file_header_t mandatory;
        ezb_zcl_ota_file_header_optional_t optional;
    } __attribute__((packed)) header;
    struct {
        uint16_t type;
        uint32_t total;
        uint16_t length;
        uint8_t *val;
    } __attribute__((packed)) element;
} esp_zb_ota_file_parser_t;

esp_zb_ota_file_parser_t *esp_zb_create_ota_file_parser(uint32_t total_image_size);
void esp_zb_free_ota_file_parser(esp_zb_ota_file_parser_t *parser);
void esp_zb_ota_file_parser_setup(esp_zb_ota_file_parser_t *parser,
                                  uint32_t block_size,
                                  uint8_t *block);
bool esp_zb_ota_file_parser_is_element_value(esp_zb_ota_file_parser_t *parser);
esp_err_t esp_zb_ota_file_parser_process(esp_zb_ota_file_parser_t *parser);
esp_err_t esp_zb_ota_file_parser_check(esp_zb_ota_file_parser_t *parser);
