#include "soil_model.h"

#include <math.h>
#include <string.h>

#define SOIL_ADC_MIN_MV 50.0f
#define SOIL_ADC_MAX_MV 3300.0f
#define SOIL_MIN_CALIBRATION_SPAN_MV 100.0f
#define SOIL_MIN_LEARNED_SPAN_MV 300.0f
#define SOIL_MIN_LEARNING_CYCLES 2U
#define SOIL_WATERING_RAW_DROP_MV 120.0f
#define SOIL_RELOCATION_JUMP_MV 1200.0f

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

static bool bounds_valid(float dry_mv, float wet_mv, float min_span)
{
    return isfinite(dry_mv) && isfinite(wet_mv) &&
           dry_mv >= SOIL_ADC_MIN_MV && dry_mv <= SOIL_ADC_MAX_MV &&
           wet_mv >= SOIL_ADC_MIN_MV && wet_mv <= SOIL_ADC_MAX_MV &&
           dry_mv > wet_mv && (dry_mv - wet_mv) >= min_span;
}

static void policy_touch(soil_policy_t *policy)
{
    if (!policy) return;
    policy->config_revision = policy->config_revision == UINT32_MAX
                                  ? 1U
                                  : policy->config_revision + 1U;
}

static void apply_battery_mode(soil_state_t *state)
{
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
        .manual_dry_raw_mv = 2750.0f,
        .manual_wet_raw_mv = 1200.0f,
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
        .config_revision = 1U,
        .calibration_mode = SOIL_CALIBRATION_STOCK,
    };
}

bool soil_policy_validate(const soil_policy_t *policy, soil_config_result_t *result)
{
    soil_config_result_t local = SOIL_CONFIG_OK;
    if (!policy || policy->calibration_mode > SOIL_CALIBRATION_MANUAL) {
        local = SOIL_CONFIG_REJECT_MODE;
    } else if (!bounds_valid(policy->dry_raw_mv, policy->wet_raw_mv,
                             SOIL_MIN_CALIBRATION_SPAN_MV) ||
               !bounds_valid(policy->manual_dry_raw_mv, policy->manual_wet_raw_mv,
                             SOIL_MIN_CALIBRATION_SPAN_MV)) {
        local = SOIL_CONFIG_REJECT_SPAN;
    } else if (!isfinite(policy->dry_threshold_pct) ||
               !isfinite(policy->critical_threshold_pct) ||
               policy->critical_threshold_pct < 0.0f ||
               policy->dry_threshold_pct > 100.0f ||
               policy->critical_threshold_pct >= policy->dry_threshold_pct) {
        local = SOIL_CONFIG_REJECT_THRESHOLDS;
    }
    if (result) *result = local;
    return local == SOIL_CONFIG_OK;
}

soil_config_result_t soil_policy_set_mode(soil_policy_t *policy, soil_calibration_mode_t mode)
{
    if (!policy || mode > SOIL_CALIBRATION_MANUAL) return SOIL_CONFIG_REJECT_MODE;
    soil_policy_t candidate = *policy;
    candidate.calibration_mode = mode;
    soil_config_result_t result;
    if (!soil_policy_validate(&candidate, &result)) return result;
    *policy = candidate;
    policy_touch(policy);
    return SOIL_CONFIG_OK;
}

soil_config_result_t soil_policy_set_manual_bounds(soil_policy_t *policy, float dry_mv, float wet_mv)
{
    if (!policy) return SOIL_CONFIG_REJECT_ADC_RANGE;
    if (!isfinite(dry_mv) || !isfinite(wet_mv) ||
        dry_mv < SOIL_ADC_MIN_MV || dry_mv > SOIL_ADC_MAX_MV ||
        wet_mv < SOIL_ADC_MIN_MV || wet_mv > SOIL_ADC_MAX_MV) {
        return SOIL_CONFIG_REJECT_ADC_RANGE;
    }
    if (!bounds_valid(dry_mv, wet_mv, SOIL_MIN_CALIBRATION_SPAN_MV)) {
        return SOIL_CONFIG_REJECT_SPAN;
    }
    policy->manual_dry_raw_mv = dry_mv;
    policy->manual_wet_raw_mv = wet_mv;
    policy_touch(policy);
    return SOIL_CONFIG_OK;
}

soil_config_result_t soil_policy_set_thresholds(soil_policy_t *policy, float dry_pct, float critical_pct)
{
    if (!policy || !isfinite(dry_pct) || !isfinite(critical_pct) ||
        critical_pct < 0.0f || dry_pct > 100.0f || critical_pct >= dry_pct) {
        return SOIL_CONFIG_REJECT_THRESHOLDS;
    }
    policy->dry_threshold_pct = dry_pct;
    policy->critical_threshold_pct = critical_pct;
    policy_touch(policy);
    return SOIL_CONFIG_OK;
}

soil_config_result_t soil_policy_apply_configuration(
    soil_policy_t *policy,
    soil_calibration_mode_t mode,
    float manual_dry_mv,
    float manual_wet_mv,
    float dry_threshold_pct,
    float critical_threshold_pct,
    uint32_t revision)
{
    if (!policy) return SOIL_CONFIG_REJECT_PERSISTENCE;
    if (revision <= policy->config_revision) return SOIL_CONFIG_OK;

    soil_policy_t candidate = *policy;
    candidate.calibration_mode = mode;
    candidate.manual_dry_raw_mv = manual_dry_mv;
    candidate.manual_wet_raw_mv = manual_wet_mv;
    candidate.dry_threshold_pct = dry_threshold_pct;
    candidate.critical_threshold_pct = critical_threshold_pct;
    candidate.config_revision = revision;

    soil_config_result_t result;
    if (!soil_policy_validate(&candidate, &result)) return result;
    *policy = candidate;
    return SOIL_CONFIG_OK;
}

void soil_learning_reset(soil_state_t *state)
{
    if (!state) return;
    state->learning_has_dry_candidate = false;
    state->learning_has_wet_candidate = false;
    state->learned_dry_raw_mv = 0.0f;
    state->learned_wet_raw_mv = 0.0f;
    state->learning_dry_candidate_mv = 0.0f;
    state->learning_wet_candidate_mv = 0.0f;
    state->learning_confidence_pct = 0.0f;
    state->learning_cycle_count = 0U;
}

void soil_resolve_active_curve(const soil_policy_t *policy, soil_state_t *state)
{
    if (!policy || !state) return;

    state->active_dry_raw_mv = policy->dry_raw_mv;
    state->active_wet_raw_mv = policy->wet_raw_mv;
    state->active_curve_source = SOIL_CURVE_STOCK;

    if (policy->calibration_mode == SOIL_CALIBRATION_MANUAL) {
        if (bounds_valid(policy->manual_dry_raw_mv, policy->manual_wet_raw_mv,
                         SOIL_MIN_CALIBRATION_SPAN_MV)) {
            state->active_dry_raw_mv = policy->manual_dry_raw_mv;
            state->active_wet_raw_mv = policy->manual_wet_raw_mv;
            state->active_curve_source = SOIL_CURVE_MANUAL;
        } else {
            state->active_curve_source = SOIL_CURVE_FALLBACK_STOCK;
        }
        return;
    }

    if (policy->calibration_mode == SOIL_CALIBRATION_LEARNING) {
        if (state->learning_cycle_count >= SOIL_MIN_LEARNING_CYCLES &&
            state->learning_confidence_pct >= 40.0f &&
            bounds_valid(state->learned_dry_raw_mv, state->learned_wet_raw_mv,
                         SOIL_MIN_LEARNED_SPAN_MV)) {
            const float blend = clampf((state->learning_confidence_pct - 40.0f) / 60.0f,
                                       0.0f, 1.0f);
            state->active_dry_raw_mv =
                policy->dry_raw_mv * (1.0f - blend) + state->learned_dry_raw_mv * blend;
            state->active_wet_raw_mv =
                policy->wet_raw_mv * (1.0f - blend) + state->learned_wet_raw_mv * blend;
            state->active_curve_source = SOIL_CURVE_LEARNED;
        } else {
            state->active_curve_source = SOIL_CURVE_FALLBACK_STOCK;
        }
    }
}

static void update_learning(const soil_policy_t *policy,
                            const soil_sample_t *sample,
                            soil_state_t *state,
                            float stock_moisture,
                            bool watering_jump)
{
    if (policy->calibration_mode != SOIL_CALIBRATION_LEARNING ||
        !state->current_sample_valid || state->confidence_pct <= 0.0f) {
        return;
    }

    if (state->previous_raw_mv > 0.0f &&
        fabsf(sample->raw_mv - state->previous_raw_mv) > SOIL_RELOCATION_JUMP_MV &&
        !watering_jump) {
        soil_learning_reset(state);
        return;
    }

    if (stock_moisture <= policy->dry_threshold_pct &&
        (!state->has_watered || state->seconds_since_watering >= 6u * 60u * 60u)) {
        if (!state->learning_has_dry_candidate ||
            sample->raw_mv > state->learning_dry_candidate_mv) {
            state->learning_dry_candidate_mv = sample->raw_mv;
        } else {
            state->learning_dry_candidate_mv =
                0.9f * state->learning_dry_candidate_mv + 0.1f * sample->raw_mv;
        }
        state->learning_has_dry_candidate = true;
    }

    if (watering_jump) {
        state->learning_wet_candidate_mv = sample->raw_mv;
        state->learning_has_wet_candidate = true;

        if (state->learning_has_dry_candidate &&
            bounds_valid(state->learning_dry_candidate_mv,
                         state->learning_wet_candidate_mv,
                         SOIL_MIN_LEARNED_SPAN_MV)) {
            if (state->learning_cycle_count == 0U) {
                state->learned_dry_raw_mv = state->learning_dry_candidate_mv;
                state->learned_wet_raw_mv = state->learning_wet_candidate_mv;
            } else {
                state->learned_dry_raw_mv =
                    0.75f * state->learned_dry_raw_mv +
                    0.25f * state->learning_dry_candidate_mv;
                state->learned_wet_raw_mv =
                    0.75f * state->learned_wet_raw_mv +
                    0.25f * state->learning_wet_candidate_mv;
            }
            if (state->learning_cycle_count < UINT32_MAX) {
                state->learning_cycle_count++;
            }
            const float span = state->learned_dry_raw_mv - state->learned_wet_raw_mv;
            const float span_bonus = clampf((span - SOIL_MIN_LEARNED_SPAN_MV) / 20.0f,
                                            0.0f, 25.0f);
            state->learning_confidence_pct =
                clampf(state->learning_cycle_count * 25.0f + span_bonus, 0.0f, 100.0f);
            state->learning_has_dry_candidate = false;
        }
    } else if ((state->mode == SOIL_MODE_WATERING_CAPTURE ||
                state->mode == SOIL_MODE_RECENTLY_WATERED) &&
               state->learning_has_wet_candidate &&
               sample->raw_mv < state->learning_wet_candidate_mv) {
        state->learning_wet_candidate_mv =
            0.9f * state->learning_wet_candidate_mv + 0.1f * sample->raw_mv;
        if (state->learning_cycle_count > 0U) {
            state->learned_wet_raw_mv =
                0.95f * state->learned_wet_raw_mv + 0.05f * sample->raw_mv;
        }
    }
}

soil_config_result_t soil_apply_control_action(
    soil_policy_t *policy,
    soil_state_t *state,
    soil_control_action_t action,
    float current_raw_mv)
{
    if (!policy || !state) return SOIL_CONFIG_REJECT_ADC_RANGE;

    soil_config_result_t result = SOIL_CONFIG_OK;
    switch (action) {
    case SOIL_ACTION_USE_CURRENT_AS_DRY:
        if (!isfinite(current_raw_mv)) {
            result = SOIL_CONFIG_REJECT_NO_SAMPLE;
        } else {
            result = soil_policy_set_manual_bounds(
                policy, current_raw_mv, policy->manual_wet_raw_mv);
        }
        break;
    case SOIL_ACTION_USE_CURRENT_AS_WET:
        if (!isfinite(current_raw_mv)) {
            result = SOIL_CONFIG_REJECT_NO_SAMPLE;
        } else {
            result = soil_policy_set_manual_bounds(
                policy, policy->manual_dry_raw_mv, current_raw_mv);
        }
        break;
    case SOIL_ACTION_COPY_LEARNED_TO_MANUAL:
        if (!bounds_valid(state->learned_dry_raw_mv, state->learned_wet_raw_mv,
                          SOIL_MIN_LEARNED_SPAN_MV)) {
            result = SOIL_CONFIG_REJECT_NO_LEARNED_CURVE;
        } else {
            result = soil_policy_set_manual_bounds(
                policy, state->learned_dry_raw_mv, state->learned_wet_raw_mv);
        }
        break;
    case SOIL_ACTION_RESET_LEARNING:
    case SOIL_ACTION_PLANT_MOVED:
        soil_learning_reset(state);
        policy_touch(policy);
        break;
    case SOIL_ACTION_RESTORE_MANUAL_STOCK:
        result = soil_policy_set_manual_bounds(
            policy, policy->dry_raw_mv, policy->wet_raw_mv);
        break;
    case SOIL_ACTION_IDENTIFY:
    case SOIL_ACTION_NONE:
        break;
    default:
        result = SOIL_CONFIG_REJECT_MODE;
        break;
    }

    state->last_config_result = result;
    if (result == SOIL_CONFIG_OK) {
        state->applied_config_revision = policy->config_revision;
        state->event_flags |= SOIL_EVENT_CONFIG;
        state->should_report = true;
        state->seconds_since_report = 0U;
        soil_resolve_active_curve(policy, state);
    }
    return result;
}

float soil_calibrated_percent(float raw_mv, float dry_mv, float wet_mv)
{
    const float span = dry_mv - wet_mv;
    if (!isfinite(raw_mv) || !isfinite(span) ||
        fabsf(span) < SOIL_MIN_CALIBRATION_SPAN_MV) {
        return NAN;
    }
    return clampf((dry_mv - raw_mv) * 100.0f / span, 0.0f, 100.0f);
}

float soil_battery_percent(float battery_mv)
{
    static const struct {
        float mv;
        float pct;
    } curve[] = {
        {1000.0f, 0.0f}, {1100.0f, 5.0f}, {1180.0f, 15.0f},
        {1250.0f, 35.0f}, {1320.0f, 60.0f}, {1400.0f, 82.0f},
        {1500.0f, 100.0f},
    };
    if (!isfinite(battery_mv)) return 0.0f;
    if (battery_mv <= curve[0].mv) return 0.0f;
    for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
        if (battery_mv <= curve[i].mv) {
            const float ratio =
                (battery_mv - curve[i - 1].mv) /
                (curve[i].mv - curve[i - 1].mv);
            return curve[i - 1].pct +
                   ratio * (curve[i].pct - curve[i - 1].pct);
        }
    }
    return 100.0f;
}

void soil_model_step(const soil_policy_t *policy,
                     const soil_sample_t *sample,
                     soil_state_t *state)
{
    if (!policy || !sample || !state) return;

    const bool was_initialized = state->initialized;
    const bool previous_fault = state->sensor_fault;
    const bool previous_battery_present = state->battery_present;
    const float previous_battery_pct = state->battery_pct;
    const soil_mode_t previous_mode = state->mode;
    const float previous_raw_mv = state->previous_raw_mv;

    soil_resolve_active_curve(policy, state);
    const float moisture = soil_calibrated_percent(
        sample->raw_mv, state->active_dry_raw_mv, state->active_wet_raw_mv);
    const float stock_moisture = soil_calibrated_percent(
        sample->raw_mv, policy->dry_raw_mv, policy->wet_raw_mv);
    const bool calibration_fault = !isfinite(moisture);
    const bool electrical_fault =
        !isfinite(sample->raw_mv) || sample->raw_mv < SOIL_ADC_MIN_MV ||
        sample->raw_mv > SOIL_ADC_MAX_MV;
    const bool hard_fault = calibration_fault || electrical_fault;
    const bool noise_fault =
        !isfinite(sample->noise_mv) || sample->noise_mv > policy->noise_fault_mv;
    const bool fault_now = hard_fault || noise_fault;

    state->event_flags = SOIL_EVENT_NONE;
    state->should_report = false;
    state->current_sample_valid = !hard_fault;
    state->battery_present = sample->battery_present;
    state->applied_config_revision = policy->config_revision;
    state->seconds_since_report =
        saturating_add_u32(state->seconds_since_report, sample->elapsed_seconds);
    if (state->has_watered) {
        state->seconds_since_watering =
            saturating_add_u32(state->seconds_since_watering,
                               sample->elapsed_seconds);
    }

    if (sample->battery_present) {
        state->battery_pct = soil_battery_percent(sample->battery_mv);
    }

    if (fault_now != previous_fault || (!was_initialized && fault_now)) {
        state->event_flags |= SOIL_EVENT_FAULT;
    }
    state->sensor_fault = fault_now;

    if (sample->battery_present) {
        const bool battery_state_changed =
            !previous_battery_present ||
            (previous_battery_pct > 20.0f && state->battery_pct <= 20.0f) ||
            (previous_battery_pct > 8.0f && state->battery_pct <= 8.0f);
        if (battery_state_changed) state->event_flags |= SOIL_EVENT_BATTERY;
    }

    if (hard_fault) {
        state->initialized = true;
        state->confidence_pct = 0.0f;
        state->mode =
            sample->battery_present && state->battery_pct <= 8.0f
                ? SOIL_MODE_SURVIVAL
                : SOIL_MODE_CONSERVATION;
        state->sample_interval_seconds =
            state->mode == SOIL_MODE_SURVIVAL ? 12u * 60u * 60u
                                              : 6u * 60u * 60u;
        if (state->seconds_since_report >= policy->heartbeat_seconds)
            state->event_flags |= SOIL_EVENT_HEARTBEAT;
        if (sample->manual_sample) state->event_flags |= SOIL_EVENT_MANUAL;
        if (was_initialized && state->mode != previous_mode)
            state->event_flags |= SOIL_EVENT_MODE;
        state->should_report = state->event_flags != SOIL_EVENT_NONE;
        if (state->should_report) state->seconds_since_report = 0U;
        return;
    }

    if (!was_initialized || !state->has_valid_moisture) {
        state->initialized = true;
        state->has_valid_moisture = true;
        state->moisture_pct = moisture;
        state->previous_moisture_pct = moisture;
        state->previous_raw_mv = sample->raw_mv;
        state->last_reported_pct = moisture;
        state->drying_rate_pct_per_hour = 0.0f;
        state->confidence_pct =
            noise_fault ? 0.0f
                        : clampf(100.0f - sample->noise_mv, 0.0f, 100.0f);
        state->mode =
            moisture <= policy->critical_threshold_pct
                ? SOIL_MODE_CRITICAL
                : moisture <= policy->dry_threshold_pct ? SOIL_MODE_NEAR_DRY
                                                        : SOIL_MODE_STABLE;
        state->sample_interval_seconds =
            state->mode == SOIL_MODE_CRITICAL
                ? policy->critical_sample_seconds
                : state->mode == SOIL_MODE_NEAR_DRY
                      ? policy->near_dry_sample_seconds
                      : policy->stable_sample_seconds;
        apply_battery_mode(state);
        update_learning(policy, sample, state, stock_moisture, false);
        state->event_flags |= SOIL_EVENT_HEARTBEAT;
        if (sample->manual_sample) state->event_flags |= SOIL_EVENT_MANUAL;
        state->should_report = true;
        state->seconds_since_report = 0U;
        return;
    }

    state->previous_moisture_pct = state->moisture_pct;
    state->moisture_pct = moisture;
    const float delta = moisture - state->previous_moisture_pct;
    const bool watering_jump =
        delta >= policy->watering_delta_pct ||
        (previous_raw_mv > 0.0f &&
         previous_raw_mv - sample->raw_mv >= SOIL_WATERING_RAW_DROP_MV);

    if (sample->elapsed_seconds > 0U) {
        const float hours = sample->elapsed_seconds / 3600.0f;
        const float instantaneous_rate = -delta / hours;
        state->drying_rate_pct_per_hour =
            0.75f * state->drying_rate_pct_per_hour +
            0.25f * instantaneous_rate;
    }
    state->confidence_pct =
        noise_fault
            ? 0.0f
            : clampf(100.0f - sample->noise_mv * 0.8f, 0.0f, 100.0f);

    if (watering_jump) {
        state->mode = SOIL_MODE_WATERING_CAPTURE;
        state->sample_interval_seconds = policy->watering_sample_seconds;
        state->has_watered = true;
        state->seconds_since_watering = 0U;
        state->event_flags |= SOIL_EVENT_WATERING;
    } else if (previous_mode == SOIL_MODE_WATERING_CAPTURE) {
        state->mode = SOIL_MODE_RECENTLY_WATERED;
        state->sample_interval_seconds = policy->recent_water_sample_seconds;
    } else if (previous_mode == SOIL_MODE_RECENTLY_WATERED &&
               state->seconds_since_watering < 60u * 60u) {
        state->mode = SOIL_MODE_RECENTLY_WATERED;
        state->sample_interval_seconds = policy->recent_water_sample_seconds;
    } else if (moisture <= policy->critical_threshold_pct) {
        if (previous_mode != SOIL_MODE_CRITICAL)
            state->event_flags |= SOIL_EVENT_THRESHOLD;
        state->mode = SOIL_MODE_CRITICAL;
        state->sample_interval_seconds = policy->critical_sample_seconds;
    } else if (moisture <= policy->dry_threshold_pct) {
        if (previous_mode != SOIL_MODE_NEAR_DRY)
            state->event_flags |= SOIL_EVENT_THRESHOLD;
        state->mode = SOIL_MODE_NEAR_DRY;
        state->sample_interval_seconds = policy->near_dry_sample_seconds;
    } else if (state->drying_rate_pct_per_hour > 0.2f) {
        state->mode = SOIL_MODE_DRYING;
        state->sample_interval_seconds = policy->drying_sample_seconds;
    } else {
        state->mode = SOIL_MODE_STABLE;
        state->sample_interval_seconds = policy->stable_sample_seconds;
    }

    update_learning(policy, sample, state, stock_moisture, watering_jump);
    soil_resolve_active_curve(policy, state);
    state->previous_raw_mv = sample->raw_mv;
    apply_battery_mode(state);

    if (state->mode != previous_mode) state->event_flags |= SOIL_EVENT_MODE;
    if (fabsf(moisture - state->last_reported_pct) >=
        policy->report_delta_pct) {
        state->event_flags |= SOIL_EVENT_THRESHOLD;
    }
    if (state->seconds_since_report >= policy->heartbeat_seconds)
        state->event_flags |= SOIL_EVENT_HEARTBEAT;
    if (sample->manual_sample) state->event_flags |= SOIL_EVENT_MANUAL;

    state->should_report = state->event_flags != SOIL_EVENT_NONE;
    if (state->should_report) {
        state->last_reported_pct = moisture;
        state->seconds_since_report = 0U;
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

const char *soil_calibration_mode_name(soil_calibration_mode_t mode)
{
    switch (mode) {
    case SOIL_CALIBRATION_STOCK: return "stock";
    case SOIL_CALIBRATION_LEARNING: return "learning";
    case SOIL_CALIBRATION_MANUAL: return "manual";
    default: return "unknown";
    }
}

const char *soil_curve_source_name(soil_curve_source_t source)
{
    switch (source) {
    case SOIL_CURVE_STOCK: return "stock";
    case SOIL_CURVE_LEARNED: return "learned";
    case SOIL_CURVE_MANUAL: return "manual";
    case SOIL_CURVE_FALLBACK_STOCK: return "fallback_stock";
    default: return "unknown";
    }
}
