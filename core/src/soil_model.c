#include "soil_model.h"

#include <math.h>

static float clampf(float value, float low, float high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static uint32_t saturating_add_u32(uint32_t value, uint32_t increment)
{
    return UINT32_MAX - value < increment ? UINT32_MAX : value + increment;
}

static void apply_battery_mode(const soil_policy_t *policy, soil_state_t *state)
{
    (void)policy;
    if (!state->battery_present) return;

    if (state->battery_pct <= 8.0f) {
        state->mode = SOIL_MODE_SURVIVAL;
        state->sample_interval_seconds = 12u * 60u * 60u;
    } else if (state->battery_pct <= 20.0f && state->mode == SOIL_MODE_STABLE) {
        state->mode = SOIL_MODE_CONSERVATION;
        state->sample_interval_seconds = 12u * 60u * 60u;
    }
}

soil_policy_t soil_policy_default(void)
{
    return (soil_policy_t){
        .dry_raw_mv = 2750.0f,
        .wet_raw_mv = 1200.0f,
        .dry_threshold_pct = 28.0f,
        .critical_threshold_pct = 16.0f,
        .report_delta_pct = 3.0f,
        .watering_delta_pct = 8.0f,
        .noise_fault_mv = 90.0f,
        .heartbeat_seconds = 24u * 60u * 60u,
        .stable_sample_seconds = 4u * 60u * 60u,
        .drying_sample_seconds = 60u * 60u,
        .near_dry_sample_seconds = 30u * 60u,
        .critical_sample_seconds = 15u * 60u,
        .watering_sample_seconds = 2u * 60u,
        .recent_water_sample_seconds = 10u * 60u,
    };
}

float soil_calibrated_percent(float raw_mv, float dry_mv, float wet_mv)
{
    const float span = dry_mv - wet_mv;
    if (!isfinite(raw_mv) || !isfinite(span) || fabsf(span) < 100.0f) return NAN;
    return clampf((dry_mv - raw_mv) * 100.0f / span, 0.0f, 100.0f);
}

float soil_battery_percent(float battery_mv)
{
    /* Conservative alkaline AA estimate under the carrier boost-converter load. */
    static const struct { float mv; float pct; } curve[] = {
        {1000.0f, 0.0f}, {1100.0f, 5.0f}, {1180.0f, 15.0f},
        {1250.0f, 35.0f}, {1320.0f, 60.0f}, {1400.0f, 82.0f},
        {1500.0f, 100.0f},
    };
    if (!isfinite(battery_mv)) return 0.0f;
    if (battery_mv <= curve[0].mv) return 0.0f;
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
        if (battery_mv <= curve[i].mv) {
            const float ratio = (battery_mv - curve[i - 1].mv) / (curve[i].mv - curve[i - 1].mv);
            return curve[i - 1].pct + ratio * (curve[i].pct - curve[i - 1].pct);
        }
    }
    return 100.0f;
}

void soil_model_step(const soil_policy_t *policy, const soil_sample_t *sample, soil_state_t *state)
{
    if (!policy || !sample || !state) return;

    const bool was_initialized = state->initialized;
    const bool previous_fault = state->sensor_fault;
    const bool previous_battery_present = state->battery_present;
    const float previous_battery_pct = state->battery_pct;
    const soil_mode_t previous_mode = state->mode;

    const float moisture = soil_calibrated_percent(sample->raw_mv, policy->dry_raw_mv, policy->wet_raw_mv);
    const bool calibration_fault = !isfinite(moisture);
    const bool electrical_fault = !isfinite(sample->raw_mv) || sample->raw_mv < 50.0f || sample->raw_mv > 3300.0f;
    const bool hard_fault = calibration_fault || electrical_fault;
    const bool noise_fault = !isfinite(sample->noise_mv) || sample->noise_mv > policy->noise_fault_mv;
    const bool fault_now = hard_fault || noise_fault;

    state->event_flags = SOIL_EVENT_NONE;
    state->should_report = false;
    state->current_sample_valid = !hard_fault;
    state->battery_present = sample->battery_present;
    state->seconds_since_report = saturating_add_u32(state->seconds_since_report, sample->elapsed_seconds);
    if (state->has_watered) {
        state->seconds_since_watering = saturating_add_u32(state->seconds_since_watering, sample->elapsed_seconds);
    }

    if (sample->battery_present) {
        state->battery_pct = soil_battery_percent(sample->battery_mv);
    }

    if (fault_now != previous_fault || (!was_initialized && fault_now)) {
        state->event_flags |= SOIL_EVENT_FAULT;
    }
    state->sensor_fault = fault_now;

    if (sample->battery_present) {
        const bool battery_state_changed = !previous_battery_present ||
                                           (previous_battery_pct > 20.0f && state->battery_pct <= 20.0f) ||
                                           (previous_battery_pct > 8.0f && state->battery_pct <= 8.0f);
        if (battery_state_changed) {
            state->event_flags |= SOIL_EVENT_BATTERY;
        }
    }

    if (hard_fault) {
        state->initialized = true;
        state->confidence_pct = 0.0f;
        state->mode = sample->battery_present && state->battery_pct <= 8.0f
                          ? SOIL_MODE_SURVIVAL
                          : SOIL_MODE_CONSERVATION;
        state->sample_interval_seconds = state->mode == SOIL_MODE_SURVIVAL
                                             ? 12u * 60u * 60u
                                             : 6u * 60u * 60u;

        if (state->seconds_since_report >= policy->heartbeat_seconds) {
            state->event_flags |= SOIL_EVENT_HEARTBEAT;
        }
        if (sample->manual_sample) {
            state->event_flags |= SOIL_EVENT_MANUAL;
        }
        state->should_report = state->event_flags != SOIL_EVENT_NONE;
        if (state->should_report) state->seconds_since_report = 0;
        return;
    }

    if (!was_initialized || !state->has_valid_moisture) {
        state->initialized = true;
        state->has_valid_moisture = true;
        state->moisture_pct = moisture;
        state->previous_moisture_pct = moisture;
        state->last_reported_pct = moisture;
        state->drying_rate_pct_per_hour = 0.0f;
        state->confidence_pct = noise_fault ? 0.0f : clampf(100.0f - sample->noise_mv, 0.0f, 100.0f);
        state->mode = moisture <= policy->critical_threshold_pct ? SOIL_MODE_CRITICAL
                    : moisture <= policy->dry_threshold_pct ? SOIL_MODE_NEAR_DRY
                    : SOIL_MODE_STABLE;
        state->sample_interval_seconds = state->mode == SOIL_MODE_CRITICAL ? policy->critical_sample_seconds
                                       : state->mode == SOIL_MODE_NEAR_DRY ? policy->near_dry_sample_seconds
                                       : policy->stable_sample_seconds;
        apply_battery_mode(policy, state);
        state->event_flags |= SOIL_EVENT_HEARTBEAT;
        if (sample->manual_sample) state->event_flags |= SOIL_EVENT_MANUAL;
        state->should_report = true;
        state->seconds_since_report = 0;
        return;
    }

    state->previous_moisture_pct = state->moisture_pct;
    state->moisture_pct = moisture;
    const float delta = moisture - state->previous_moisture_pct;

    if (sample->elapsed_seconds > 0) {
        const float hours = sample->elapsed_seconds / 3600.0f;
        const float instantaneous_rate = -delta / hours;
        state->drying_rate_pct_per_hour = 0.75f * state->drying_rate_pct_per_hour + 0.25f * instantaneous_rate;
    }
    state->confidence_pct = noise_fault ? 0.0f : clampf(100.0f - sample->noise_mv * 0.8f, 0.0f, 100.0f);

    if (delta >= policy->watering_delta_pct) {
        state->mode = SOIL_MODE_WATERING_CAPTURE;
        state->sample_interval_seconds = policy->watering_sample_seconds;
        state->has_watered = true;
        state->seconds_since_watering = 0;
        state->event_flags |= SOIL_EVENT_WATERING;
    } else if (previous_mode == SOIL_MODE_WATERING_CAPTURE) {
        state->mode = SOIL_MODE_RECENTLY_WATERED;
        state->sample_interval_seconds = policy->recent_water_sample_seconds;
    } else if (previous_mode == SOIL_MODE_RECENTLY_WATERED && state->seconds_since_watering < 60u * 60u) {
        state->mode = SOIL_MODE_RECENTLY_WATERED;
        state->sample_interval_seconds = policy->recent_water_sample_seconds;
    } else if (moisture <= policy->critical_threshold_pct) {
        if (previous_mode != SOIL_MODE_CRITICAL) state->event_flags |= SOIL_EVENT_THRESHOLD;
        state->mode = SOIL_MODE_CRITICAL;
        state->sample_interval_seconds = policy->critical_sample_seconds;
    } else if (moisture <= policy->dry_threshold_pct) {
        if (previous_mode != SOIL_MODE_NEAR_DRY) state->event_flags |= SOIL_EVENT_THRESHOLD;
        state->mode = SOIL_MODE_NEAR_DRY;
        state->sample_interval_seconds = policy->near_dry_sample_seconds;
    } else if (state->drying_rate_pct_per_hour > 0.2f) {
        state->mode = SOIL_MODE_DRYING;
        state->sample_interval_seconds = policy->drying_sample_seconds;
    } else {
        state->mode = SOIL_MODE_STABLE;
        state->sample_interval_seconds = policy->stable_sample_seconds;
    }

    apply_battery_mode(policy, state);

    if (fabsf(moisture - state->last_reported_pct) >= policy->report_delta_pct) {
        state->event_flags |= SOIL_EVENT_THRESHOLD;
    }
    if (state->seconds_since_report >= policy->heartbeat_seconds) {
        state->event_flags |= SOIL_EVENT_HEARTBEAT;
    }
    if (sample->manual_sample) {
        state->event_flags |= SOIL_EVENT_MANUAL;
    }

    state->should_report = state->event_flags != SOIL_EVENT_NONE;
    if (state->should_report) {
        state->last_reported_pct = moisture;
        state->seconds_since_report = 0;
    }
}

const char *soil_mode_name(soil_mode_t mode)
{
    switch (mode) {
    case SOIL_MODE_STABLE: return "stable";
    case SOIL_MODE_DRYING: return "drying";
    case SOIL_MODE_NEAR_DRY: return "near_dry";
    case SOIL_MODE_CRITICAL: return "critical";
    case SOIL_MODE_WATERING_CAPTURE: return "watering_capture";
    case SOIL_MODE_RECENTLY_WATERED: return "recently_watered";
    case SOIL_MODE_CONSERVATION: return "conservation";
    case SOIL_MODE_SURVIVAL: return "survival";
    default: return "unknown";
    }
}
