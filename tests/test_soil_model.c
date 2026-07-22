#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "soil_model.h"
#include "soil_service.h"
#include "soil_telemetry.h"

static soil_sample_t sample(float raw_mv, float battery_mv, float noise_mv)
{
    return (soil_sample_t){
        .raw_mv = raw_mv,
        .battery_mv = battery_mv,
        .noise_mv = noise_mv,
        .battery_present = true,
    };
}

static void test_calibration(void)
{
    assert(fabsf(soil_calibrated_percent(2750, 2750, 1200) - 0.0f) < 0.01f);
    assert(fabsf(soil_calibrated_percent(1200, 2750, 1200) - 100.0f) < 0.01f);
    assert(isnan(soil_calibrated_percent(2000, 2000, 1950)));
}

static void test_policy_defaults_and_validation(void)
{
    soil_policy_t p = soil_policy_default();
    soil_config_result_t result = SOIL_CONFIG_REJECT_MODE;
    assert(soil_policy_validate(&p, &result));
    assert(result == SOIL_CONFIG_OK);
    assert(p.calibration_mode == SOIL_CALIBRATION_STOCK);
    assert(p.config_revision == 1U);
    assert(p.manual_dry_raw_mv == p.dry_raw_mv);
    assert(p.manual_wet_raw_mv == p.wet_raw_mv);
    assert(p.heartbeat_seconds == 24u * 60u * 60u);
    assert(p.stable_sample_seconds == 4u * 60u * 60u);
}

static void test_three_modes(void)
{
    soil_policy_t p = soil_policy_default();
    soil_state_t s = {0};

    soil_resolve_active_curve(&p, &s);
    assert(s.active_curve_source == SOIL_CURVE_STOCK);
    assert(s.active_dry_raw_mv == p.dry_raw_mv);

    assert(soil_policy_set_manual_bounds(&p, 2600.0f, 1400.0f) == SOIL_CONFIG_OK);
    assert(soil_policy_set_mode(&p, SOIL_CALIBRATION_MANUAL) == SOIL_CONFIG_OK);
    soil_resolve_active_curve(&p, &s);
    assert(s.active_curve_source == SOIL_CURVE_MANUAL);
    assert(s.active_dry_raw_mv == 2600.0f);
    assert(s.active_wet_raw_mv == 1400.0f);

    assert(soil_policy_set_mode(&p, SOIL_CALIBRATION_LEARNING) == SOIL_CONFIG_OK);
    soil_resolve_active_curve(&p, &s);
    assert(s.active_curve_source == SOIL_CURVE_FALLBACK_STOCK);

    s.learning_cycle_count = 3U;
    s.learning_confidence_pct = 100.0f;
    s.learned_dry_raw_mv = 2500.0f;
    s.learned_wet_raw_mv = 1300.0f;
    soil_resolve_active_curve(&p, &s);
    assert(s.active_curve_source == SOIL_CURVE_LEARNED);
    assert(fabsf(s.active_dry_raw_mv - 2500.0f) < 0.01f);
}

static void test_invalid_manual_curve_is_atomic(void)
{
    soil_policy_t p = soil_policy_default();
    const uint32_t revision = p.config_revision;
    assert(soil_policy_set_manual_bounds(&p, 1300.0f, 1400.0f) ==
           SOIL_CONFIG_REJECT_SPAN);
    assert(p.manual_dry_raw_mv == 2750.0f);
    assert(p.manual_wet_raw_mv == 1200.0f);
    assert(p.config_revision == revision);
}

static void test_threshold_validation(void)
{
    soil_policy_t p = soil_policy_default();
    assert(soil_policy_set_thresholds(&p, 30.0f, 15.0f) == SOIL_CONFIG_OK);
    assert(p.dry_threshold_pct == 30.0f);
    assert(p.critical_threshold_pct == 15.0f);
    const uint32_t revision = p.config_revision;
    assert(soil_policy_set_thresholds(&p, 10.0f, 15.0f) ==
           SOIL_CONFIG_REJECT_THRESHOLDS);
    assert(p.config_revision == revision);
}

static void learning_cycle(soil_policy_t *p, soil_state_t *s,
                           float dry_raw, float wet_raw)
{
    soil_sample_t dry = sample(dry_raw, 1450.0f, 5.0f);
    dry.elapsed_seconds = 24u * 60u * 60u;
    soil_model_step(p, &dry, s);

    soil_sample_t wet = sample(wet_raw, 1450.0f, 5.0f);
    wet.elapsed_seconds = 60u;
    soil_model_step(p, &wet, s);

    soil_sample_t settled = sample(wet_raw, 1450.0f, 5.0f);
    settled.elapsed_seconds = p->watering_sample_seconds;
    soil_model_step(p, &settled, s);

    /* Advance beyond the recent-watering exclusion before the next dry anchor. */
    s->seconds_since_watering = 7u * 60u * 60u;
}

static void test_learning_converges_and_can_be_disabled(void)
{
    soil_policy_t p = soil_policy_default();
    assert(soil_policy_set_mode(&p, SOIL_CALIBRATION_LEARNING) == SOIL_CONFIG_OK);
    soil_state_t s = {0};

    soil_sample_t start = sample(2500.0f, 1450.0f, 5.0f);
    soil_model_step(&p, &start, &s);

    learning_cycle(&p, &s, 2550.0f, 1450.0f);
    learning_cycle(&p, &s, 2580.0f, 1420.0f);

    assert(s.learning_cycle_count >= 2U);
    assert(s.learning_confidence_pct >= 40.0f);
    assert(s.learned_dry_raw_mv > s.learned_wet_raw_mv);
    soil_resolve_active_curve(&p, &s);
    assert(s.active_curve_source == SOIL_CURVE_LEARNED);

    assert(soil_policy_set_mode(&p, SOIL_CALIBRATION_STOCK) == SOIL_CONFIG_OK);
    soil_resolve_active_curve(&p, &s);
    assert(s.active_curve_source == SOIL_CURVE_STOCK);
    assert(s.learning_cycle_count >= 2U); /* Switching modes does not erase. */
}

static void test_copy_learning_to_manual_and_reset(void)
{
    soil_policy_t p = soil_policy_default();
    soil_state_t s = {
        .learned_dry_raw_mv = 2600.0f,
        .learned_wet_raw_mv = 1350.0f,
        .learning_cycle_count = 3U,
        .learning_confidence_pct = 90.0f,
    };
    assert(soil_apply_control_action(
               &p, &s, SOIL_ACTION_COPY_LEARNED_TO_MANUAL, NAN) ==
           SOIL_CONFIG_OK);
    assert(p.manual_dry_raw_mv == 2600.0f);
    assert(p.manual_wet_raw_mv == 1350.0f);

    assert(soil_apply_control_action(
               &p, &s, SOIL_ACTION_RESET_LEARNING, NAN) ==
           SOIL_CONFIG_OK);
    assert(s.learning_cycle_count == 0U);
    assert(s.learned_dry_raw_mv == 0.0f);
    assert(p.manual_dry_raw_mv == 2600.0f);
}

static void test_use_current_as_bounds(void)
{
    soil_policy_t p = soil_policy_default();
    soil_state_t s = {0};

    assert(soil_apply_control_action(
               &p, &s, SOIL_ACTION_USE_CURRENT_AS_DRY, 2600.0f) ==
           SOIL_CONFIG_OK);
    assert(p.manual_dry_raw_mv == 2600.0f);

    assert(soil_apply_control_action(
               &p, &s, SOIL_ACTION_USE_CURRENT_AS_WET, 1400.0f) ==
           SOIL_CONFIG_OK);
    assert(p.manual_wet_raw_mv == 1400.0f);

    const float wet = p.manual_wet_raw_mv;
    assert(soil_apply_control_action(
               &p, &s, SOIL_ACTION_USE_CURRENT_AS_DRY, 1300.0f) ==
           SOIL_CONFIG_REJECT_SPAN);
    assert(p.manual_wet_raw_mv == wet);
}

static void test_watering_detection(void)
{
    soil_policy_t p = soil_policy_default();
    soil_state_t s = {0};
    soil_sample_t first = sample(2300, 1450, 5);
    soil_model_step(&p, &first, &s);

    soil_sample_t watered = sample(1800, 1450, 5);
    watered.elapsed_seconds = 900;
    soil_model_step(&p, &watered, &s);
    assert(s.mode == SOIL_MODE_WATERING_CAPTURE);
    assert((s.event_flags & SOIL_EVENT_WATERING) != 0);
    assert((s.event_flags & SOIL_EVENT_MODE) != 0);
    assert(s.has_watered);

    soil_sample_t settled = sample(1800, 1450, 5);
    settled.elapsed_seconds = p.watering_sample_seconds;
    soil_model_step(&p, &settled, &s);
    assert(s.mode == SOIL_MODE_RECENTLY_WATERED);
    assert((s.event_flags & SOIL_EVENT_MODE) != 0);
    assert(s.should_report);
}

static void test_low_battery_survival(void)
{
    soil_policy_t p = soil_policy_default();
    soil_state_t s = {0};
    soil_sample_t first = sample(2000, 1050, 5);
    soil_model_step(&p, &first, &s);
    assert(s.mode == SOIL_MODE_SURVIVAL);
    assert(s.sample_interval_seconds == 12u * 60u * 60u);
    assert((s.event_flags & SOIL_EVENT_BATTERY) != 0);
}

static void test_usb_without_battery_does_not_force_survival(void)
{
    soil_policy_t p = soil_policy_default();
    soil_state_t s = {0};
    soil_sample_t usb = sample(2000, 0, 5);
    usb.battery_present = false;
    usb.manual_sample = true;
    soil_model_step(&p, &usb, &s);

    assert(!s.battery_present);
    assert(s.mode != SOIL_MODE_SURVIVAL);
    assert(s.has_valid_moisture);
    assert(s.current_sample_valid);
}

static void test_hard_fault_preserves_last_valid_moisture(void)
{
    soil_policy_t p = soil_policy_default();
    soil_state_t s = {0};
    soil_sample_t valid = sample(2000, 1450, 5);
    soil_model_step(&p, &valid, &s);
    const float previous = s.moisture_pct;

    soil_sample_t broken = sample(0, 1450, 5);
    soil_model_step(&p, &broken, &s);
    assert(s.sensor_fault);
    assert(!s.current_sample_valid);
    assert(s.has_valid_moisture);
    assert(fabsf(s.moisture_pct - previous) < 0.01f);
}

static void test_telemetry_flags(void)
{
    soil_state_t s = {
        .event_flags = SOIL_EVENT_MANUAL | SOIL_EVENT_CONFIG,
        .current_sample_valid = true,
        .has_valid_moisture = true,
        .has_watered = true,
    };
    const uint16_t flags = soil_telemetry_flags(&s);
    assert((flags & SOIL_EVENT_CONFIG) != 0);
    assert((flags & SOIL_TELEMETRY_STATUS_CURRENT_SAMPLE_VALID) != 0);
    assert((flags & SOIL_TELEMETRY_STATUS_HAS_VALID_MOISTURE) != 0);
    assert((flags & SOIL_TELEMETRY_STATUS_HAS_WATERED) != 0);
}

static void test_atomic_revisioned_configuration(void)
{
    soil_policy_t p = soil_policy_default();
    const soil_policy_t original = p;

    assert(soil_policy_apply_configuration(
               &p, SOIL_CALIBRATION_MANUAL, 1300.0f, 1400.0f,
               30.0f, 15.0f, 2U) == SOIL_CONFIG_REJECT_SPAN);
    assert(memcmp(&p, &original, sizeof(p)) == 0);

    assert(soil_policy_apply_configuration(
               &p, SOIL_CALIBRATION_MANUAL, 2600.0f, 1400.0f,
               30.0f, 15.0f, 2U) == SOIL_CONFIG_OK);
    assert(p.config_revision == 2U);
    assert(p.calibration_mode == SOIL_CALIBRATION_MANUAL);
    assert(p.manual_dry_raw_mv == 2600.0f);
    assert(p.manual_wet_raw_mv == 1400.0f);

    const soil_policy_t applied = p;
    assert(soil_policy_apply_configuration(
               &p, SOIL_CALIBRATION_STOCK, 2750.0f, 1200.0f,
               28.0f, 16.0f, 1U) == SOIL_CONFIG_OK);
    assert(memcmp(&p, &applied, sizeof(p)) == 0);
}

static void test_service_button_policy(void)
{
    const float min_mv = 1250.0f;
    const uint32_t ota_ms = 3000U;
    const uint32_t reset_ms = 20000U;

    assert(soil_service_classify_button(false, 0U, true, 1500.0f,
                                        min_mv, ota_ms, reset_ms) ==
           SOIL_SERVICE_BUTTON_NONE);
    assert(soil_service_classify_button(true, 200U, true, 1500.0f,
                                        min_mv, ota_ms, reset_ms) ==
           SOIL_SERVICE_BUTTON_SAMPLE);
    assert(soil_service_classify_button(true, 3000U, true, 1400.0f,
                                        min_mv, ota_ms, reset_ms) ==
           SOIL_SERVICE_BUTTON_OTA);
    assert(soil_service_classify_button(true, 3000U, true, 1200.0f,
                                        min_mv, ota_ms, reset_ms) ==
           SOIL_SERVICE_BUTTON_OTA_REFUSED);
    assert(soil_service_classify_button(true, 3000U, false, NAN,
                                        min_mv, ota_ms, reset_ms) ==
           SOIL_SERVICE_BUTTON_OTA);
    assert(soil_service_classify_button(true, 20000U, true, 1200.0f,
                                        min_mv, ota_ms, reset_ms) ==
           SOIL_SERVICE_BUTTON_FACTORY_RESET);
}

int main(void)
{
    test_calibration();
    test_policy_defaults_and_validation();
    test_three_modes();
    test_invalid_manual_curve_is_atomic();
    test_atomic_revisioned_configuration();
    test_service_button_policy();
    test_threshold_validation();
    test_learning_converges_and_can_be_disabled();
    test_copy_learning_to_manual_and_reset();
    test_use_current_as_bounds();
    test_watering_detection();
    test_low_battery_survival();
    test_usb_without_battery_does_not_force_survival();
    test_hard_fault_preserves_last_valid_moisture();
    test_telemetry_flags();
    puts("soil_model tests passed");
    return 0;
}
