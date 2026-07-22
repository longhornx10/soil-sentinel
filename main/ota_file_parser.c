/* SPDX-License-Identifier: CC0-1.0
 * Derived from Espressif's ESP Zigbee SDK OTA client example.
 */
#include "ota_file_parser.h"

#include <stdlib.h>
#include <string.h>

#define MIN_VALUE(a, b) ((a) < (b) ? (a) : (b))

esp_zb_ota_file_parser_t *esp_zb_create_ota_file_parser(uint32_t total_image_size)
{
    esp_zb_ota_file_parser_t *parser = calloc(1, sizeof(*parser));
    if (parser) parser->total_image_size = total_image_size;
    return parser;
}

void esp_zb_free_ota_file_parser(esp_zb_ota_file_parser_t *parser)
{
    free(parser);
}

void esp_zb_ota_file_parser_setup(esp_zb_ota_file_parser_t *parser,
                                  uint32_t block_size,
                                  uint8_t *block)
{
    if (!parser) return;
    parser->in = block;
    parser->in_length = (uint16_t)block_size;
}

bool esp_zb_ota_file_parser_is_element_value(esp_zb_ota_file_parser_t *parser)
{
    return parser && parser->element.length && parser->element.val;
}

static uint16_t stream_copy(uint8_t *dst,
                            uint16_t dst_offset,
                            uint16_t dst_total,
                            const uint8_t *src,
                            uint16_t src_total)
{
    if (dst_offset >= dst_total) return 0U;
    const uint16_t size = MIN_VALUE((uint16_t)(dst_total - dst_offset), src_total);
    memcpy(dst + dst_offset, src, size);
    return size;
}

static void parser_advance(esp_zb_ota_file_parser_t *parser, uint16_t written)
{
    parser->offset += written;
    parser->total_offset += written;
    parser->in += written;
    parser->in_length -= written;
}

static ezb_zcl_ota_file_header_optional_t parse_optional(uint8_t field_control,
                                                          uint8_t *buffer)
{
    ezb_zcl_ota_file_header_optional_t optional = {0};
    uint16_t offset = 0U;
    if (field_control & EZB_ZCL_OTA_FILE_HEADER_FC_SECURITY_CREDENTIAL_VERSION_PRESENT) {
        memcpy(&optional.security_credential_version, buffer + offset, sizeof(uint8_t));
        offset += sizeof(uint8_t);
    }
    if (field_control & EZB_ZCL_OTA_FILE_HEADER_FC_DEVICE_SPECIFIC) {
        memcpy(&optional.upgrade_file_destination, buffer + offset, sizeof(ezb_extaddr_t));
        offset += sizeof(ezb_extaddr_t);
    }
    if (field_control & EZB_ZCL_OTA_FILE_HEADER_FC_HW_VERSION_PRESENT) {
        memcpy(&optional.minimum_hardware_version, buffer + offset, sizeof(uint16_t));
        offset += sizeof(uint16_t);
        memcpy(&optional.maximum_hardware_version, buffer + offset, sizeof(uint16_t));
    }
    return optional;
}

esp_err_t esp_zb_ota_file_parser_check(esp_zb_ota_file_parser_t *parser)
{
    return parser && parser->total_image_size > 0U &&
                   parser->total_offset == parser->total_image_size
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t esp_zb_ota_file_parser_process(esp_zb_ota_file_parser_t *parser)
{
    if (!parser || parser->in_length == 0U || !parser->in) {
        if (parser) {
            parser->element.length = 0U;
            parser->element.val = NULL;
        }
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t written = 0U;
    if (sizeof(parser->header.mandatory) > parser->total_offset) {
        written = stream_copy((uint8_t *)&parser->header.mandatory,
                              (uint16_t)parser->offset,
                              sizeof(parser->header.mandatory),
                              parser->in,
                              parser->in_length);
        parser_advance(parser, written);
        if (parser->total_offset >= sizeof(parser->header.mandatory)) parser->offset = 0U;
        goto exit;
    }

    if (parser->header.mandatory.hdr_length > parser->total_offset) {
        written = stream_copy((uint8_t *)&parser->header.optional,
                              (uint16_t)parser->offset,
                              (uint16_t)(parser->header.mandatory.hdr_length -
                                         sizeof(parser->header.mandatory)),
                              parser->in,
                              parser->in_length);
        parser_advance(parser, written);
        if (parser->total_offset >= parser->header.mandatory.hdr_length) {
            parser->offset = 0U;
            parser->header.optional = parse_optional(
                parser->header.mandatory.hdr_fc,
                (uint8_t *)&parser->header.optional);
        }
        goto exit;
    }

    if (parser->offset < OTA_ELEMENT_HEADER_LEN) {
        written = stream_copy((uint8_t *)&parser->element,
                              (uint16_t)parser->offset,
                              OTA_ELEMENT_HEADER_LEN,
                              parser->in,
                              parser->in_length);
        parser_advance(parser, written);
        parser->element.total += OTA_ELEMENT_HEADER_LEN;
        parser->element.length = 0U;
        parser->element.val = NULL;
        goto exit;
    }

    if (parser->offset < parser->element.total) {
        parser->element.length = MIN_VALUE(
            (uint16_t)(parser->element.total - parser->offset),
            parser->in_length);
        parser->element.val = parser->in;
        written = parser->element.length;
        parser_advance(parser, written);
        if (parser->offset >= parser->element.total) parser->offset = 0U;
    }

exit:
    return parser->in_length ? ESP_ERR_NOT_FINISHED : ESP_OK;
}
