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
    SOIL_CALIBRATION_STOCK = 0,
    SOIL_CALIBRATION_LEARNING = 1,
    SOIL_CALIBRATION_MANUAL = 2,
} soil_calibration_mode_t;

typedef enum {
    SOIL_CURVE_STOCK = 0,
    SOIL_CURVE_LEARNED = 1,
    SOIL_CURVE_MANUAL = 2,
    SOIL_CURVE_FALLBACK_STOCK = 3,
} soil_curve_source_t;

typedef enum {
    SOIL_CONFIG_OK = 0,
    SOIL_CONFIG_REJECT_MODE,
    SOIL_CONFIG_REJECT_ADC_RANGE,
    SOIL_CONFIG_REJECT_SPAN,
    SOIL_CONFIG_REJECT_THRESHOLDS,
    SOIL_CONFIG_REJECT_NO_SAMPLE,
    SOIL_CONFIG_REJECT_NO_LEARNED_CURVE,
    SOIL_CONFIG_REJECT_PERSISTENCE,
} soil_config_result_t;

typedef enum {
    SOIL_ACTION_NONE = 0,
    SOIL_ACTION_USE_CURRENT_AS_DRY = 1u << 0,
    SOIL_ACTION_USE_CURRENT_AS_WET = 1u << 1,
    SOIL_ACTION_COPY_LEARNED_TO_MANUAL = 1u << 2,
    SOIL_ACTION_RESET_LEARNING = 1u << 3,
    SOIL_ACTION_RESTORE_MANUAL_STOCK = 1u << 4,
    SOIL_ACTION_PLANT_MOVED = 1u << 5,
    SOIL_ACTION_IDENTIFY = 1u << 6,
} soil_control_action_t;

typedef enum {
    SOIL_EVENT_NONE = 0,
    SOIL_EVENT_THRESHOLD = 1u << 0,
    SOIL_EVENT_WATERING = 1u << 1,
    SOIL_EVENT_FAULT = 1u << 2,
    SOIL_EVENT_HEARTBEAT = 1u << 3,
    SOIL_EVENT_BATTERY = 1u << 4,
    SOIL_EVENT_MANUAL = 1u << 5,
    SOIL_EVENT_MODE = 1u << 6,
    SOIL_EVENT_CONFIG = 1u << 7,
} soil_event_flags_t;

typedef struct {
    float dry_raw_mv;
    float wet_raw_mv;
    float manual_dry_raw_mv;
    float manual_wet_raw_mv;
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
    uint32_t config_revision;
    soil_calibration_mode_t calibration_mode;
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
    bool current_sample_valid;
    bool battery_present;
    bool has_watered;
    bool learning_has_dry_candidate;
    bool learning_has_wet_candidate;
    float moisture_pct;
    float previous_moisture_pct;
    float previous_raw_mv;
    float last_reported_pct;
    float drying_rate_pct_per_hour;
    float confidence_pct;
    float battery_pct;
    float active_dry_raw_mv;
    float active_wet_raw_mv;
    float learned_dry_raw_mv;
    float learned_wet_raw_mv;
    float learning_dry_candidate_mv;
    float learning_wet_candidate_mv;
    float learning_confidence_pct;
    uint32_t learning_cycle_count;
    uint32_t seconds_since_report;
    uint32_t seconds_since_watering;
    uint32_t sample_interval_seconds;
    uint32_t event_flags;
    uint32_t applied_config_revision;
    uint64_t last_sample_rtc_us;
    soil_mode_t mode;
    soil_curve_source_t active_curve_source;
    soil_config_result_t last_config_result;
    bool sensor_fault;
    bool should_report;
} soil_state_t;

soil_policy_t soil_policy_default(void);
bool soil_policy_validate(const soil_policy_t *policy, soil_config_result_t *result);
soil_config_result_t soil_policy_set_mode(soil_policy_t *policy, soil_calibration_mode_t mode);
soil_config_result_t soil_policy_set_manual_bounds(soil_policy_t *policy, float dry_mv, float wet_mv);
soil_config_result_t soil_policy_set_thresholds(soil_policy_t *policy, float dry_pct, float critical_pct);
soil_config_result_t soil_policy_apply_configuration(
    soil_policy_t *policy,
    soil_calibration_mode_t mode,
    float manual_dry_mv,
    float manual_wet_mv,
    float dry_threshold_pct,
    float critical_threshold_pct,
    uint32_t revision
);
soil_config_result_t soil_apply_control_action(
    soil_policy_t *policy,
    soil_state_t *state,
    soil_control_action_t action,
    float current_raw_mv
);
void soil_learning_reset(soil_state_t *state);
void soil_resolve_active_curve(const soil_policy_t *policy, soil_state_t *state);
float soil_calibrated_percent(float raw_mv, float dry_mv, float wet_mv);
float soil_battery_percent(float battery_mv);
void soil_model_step(const soil_policy_t *policy, const soil_sample_t *sample, soil_state_t *state);
const char *soil_mode_name(soil_mode_t mode);
const char *soil_calibration_mode_name(soil_calibration_mode_t mode);
const char *soil_curve_source_name(soil_curve_source_t source);

#ifdef __cplusplus
}
#endif
