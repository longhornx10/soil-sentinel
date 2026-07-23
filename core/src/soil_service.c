#include "soil_service.h"

#include <math.h>

soil_service_button_action_t soil_service_classify_button(
    bool pressed,
    uint32_t hold_ms,
    bool battery_present,
    float battery_mv,
    float ota_min_battery_mv,
    uint32_t ota_hold_ms,
    uint32_t factory_reset_hold_ms)
{
    if (!pressed) return SOIL_SERVICE_BUTTON_NONE;
    if (hold_ms >= factory_reset_hold_ms) {
        return SOIL_SERVICE_BUTTON_FACTORY_RESET;
    }
    if (hold_ms >= ota_hold_ms) {
        const bool safe = !battery_present ||
                          (isfinite(battery_mv) &&
                           isfinite(ota_min_battery_mv) &&
                           battery_mv >= ota_min_battery_mv);
        return safe ? SOIL_SERVICE_BUTTON_OTA
                    : SOIL_SERVICE_BUTTON_OTA_REFUSED;
    }
    return SOIL_SERVICE_BUTTON_SAMPLE;
}
