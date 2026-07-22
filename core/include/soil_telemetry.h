#pragma once

#include <stdint.h>
#include "soil_model.h"

/*
 * Telemetry schema v1 reserves the high byte of the existing 16-bit flags
 * field for persistent state. This keeps the on-air payload unchanged while
 * allowing Home Assistant to distinguish invalid samples and absent history.
 */
#define SOIL_TELEMETRY_EVENT_MASK                    0x00FFu
#define SOIL_TELEMETRY_STATUS_CURRENT_SAMPLE_VALID  (1u << 8)
#define SOIL_TELEMETRY_STATUS_HAS_VALID_MOISTURE    (1u << 9)
#define SOIL_TELEMETRY_STATUS_HAS_WATERED           (1u << 10)

static inline uint16_t soil_telemetry_flags(const soil_state_t *state)
{
    if (!state) return 0U;

    uint16_t flags = (uint16_t)(state->event_flags & SOIL_TELEMETRY_EVENT_MASK);
    if (state->current_sample_valid) {
        flags |= SOIL_TELEMETRY_STATUS_CURRENT_SAMPLE_VALID;
    }
    if (state->has_valid_moisture) {
        flags |= SOIL_TELEMETRY_STATUS_HAS_VALID_MOISTURE;
    }
    if (state->has_watered) {
        flags |= SOIL_TELEMETRY_STATUS_HAS_WATERED;
    }
    return flags;
}
