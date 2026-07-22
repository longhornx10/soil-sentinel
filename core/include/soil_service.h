#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOIL_SERVICE_BUTTON_NONE = 0,
    SOIL_SERVICE_BUTTON_SAMPLE,
    SOIL_SERVICE_BUTTON_OTA,
    SOIL_SERVICE_BUTTON_OTA_REFUSED,
    SOIL_SERVICE_BUTTON_FACTORY_RESET,
} soil_service_button_action_t;

soil_service_button_action_t soil_service_classify_button(
    bool pressed,
    uint32_t hold_ms,
    bool battery_present,
    float battery_mv,
    float ota_min_battery_mv,
    uint32_t ota_hold_ms,
    uint32_t factory_reset_hold_ms
);

#ifdef __cplusplus
}
#endif
