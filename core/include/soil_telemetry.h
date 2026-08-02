#pragma once

#include <stdint.h>
#include "soil_model.h"

#define SOIL_TELEMETRY_EVENT_MASK                    0x00FFu
#define SOIL_TELEMETRY_STATUS_CURRENT_SAMPLE_VALID  (1u << 8)
#define SOIL_TELEMETRY_STATUS_HAS_VALID_MOISTURE    (1u << 9)
#define SOIL_TELEMETRY_STATUS_HAS_WATERED           (1u << 10)

/* Event flags are packed into bits 0-7 of the 16-bit telemetry field; the
   status bits live at bits 8-10. A future event bit >= 8 must fail to compile
   instead of silently colliding with the status bits. */
_Static_assert(
    (SOIL_EVENT_THRESHOLD | SOIL_EVENT_WATERING | SOIL_EVENT_FAULT |
     SOIL_EVENT_HEARTBEAT | SOIL_EVENT_BATTERY | SOIL_EVENT_MANUAL |
     SOIL_EVENT_MODE | SOIL_EVENT_CONFIG) == SOIL_TELEMETRY_EVENT_MASK,
    "event flags must fit in SOIL_TELEMETRY_EVENT_MASK (bits 0-7)");

static inline uint16_t soil_telemetry_flags(const soil_state_t *state)
{
    if (!state) return 0U;

    uint16_t flags = (uint16_t)(state->event_flags & SOIL_TELEMETRY_EVENT_MASK);
    if (state->current_sample_valid) flags |= SOIL_TELEMETRY_STATUS_CURRENT_SAMPLE_VALID;
    if (state->has_valid_moisture) flags |= SOIL_TELEMETRY_STATUS_HAS_VALID_MOISTURE;
    if (state->has_watered) flags |= SOIL_TELEMETRY_STATUS_HAS_WATERED;
    return flags;
}
