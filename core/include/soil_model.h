#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOIL_MODE_STABLE = 0,
    SOIL_MODE_DRYING,
    SOIL_MODE_NEAR_DRY,
    SOIL_MODE_CRITICAL,
    SOIL_MODE_WATERING_CAPTURE,
    SOIL_MODE_RECENTLY_WATERED,
    SOIL_MODE_CONSERVATION,
    SOIL_MODE_SURVIVAL,
} soil_mode_t;

typedef enum {
    SOIL_EVENT_NONE = 0,
    SOIL_EVENT_THRESHOLD = 1u << 0,
    SOIL_EVENT_WATERING = 1u << 1,
    SOIL_EVENT_FAULT = 1u << 2,
    SOIL_EVENT_HEARTBEAT = 1u << 3,
    SOIL_EVENT_BATTERY = 1u << 4,
    SOIL_EVENT_MANUAL = 1u << 5,
} soil_event_flags_t;

typedef struct {
    float dry_raw_mv;
    float wet_raw_mv;
    float dry_threshold_pct;
    float critical_threshold_pct;
    float report_delta_pct;
    float watering_delta_pct;
    float noise_fault_mv;
    uint32_t heartbeat_seconds;
    uint32_t stable_sample_seconds;
    uint32_t drying_sample_seconds;
    uint32_t near_dry_sample_seconds;
    uint32_t critical_sample_seconds;
    uint32_t watering_sample_seconds;
    uint32_t recent_water_sample_seconds;
} soil_policy_t;

typedef struct {
    float raw_mv;
    float battery_mv;
    float noise_mv;
    uint32_t elapsed_seconds;
    bool manual_sample;
    bool battery_present;
} soil_sample_t;

typedef struct {
    bool initialized;
    bool has_valid_moisture;
    bool battery_present;
    float moisture_pct;
    float previous_moisture_pct;
    float last_reported_pct;
    float drying_rate_pct_per_hour;
    float confidence_pct;
    float battery_pct;
    uint32_t seconds_since_report;
    uint32_t seconds_since_watering;
    uint32_t sample_interval_seconds;
    uint32_t event_flags;
    soil_mode_t mode;
    bool sensor_fault;
    bool should_report;
} soil_state_t;

soil_policy_t soil_policy_default(void);
float soil_calibrated_percent(float raw_mv, float dry_mv, float wet_mv);
float soil_battery_percent(float battery_mv);
void soil_model_step(const soil_policy_t *policy, const soil_sample_t *sample, soil_state_t *state);
const char *soil_mode_name(soil_mode_t mode);

#ifdef __cplusplus
}
#endif
