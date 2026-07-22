#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "soil_model.h"
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

static void test_power_policy_defaults(void)
{
    const soil_policy_t p = soil_policy_default();
    assert(p.heartbeat_seconds == 24u * 60u * 60u);
    assert(p.stable_sample_seconds == 4u * 60u * 60u);
    assert(p.drying_sample_seconds == 60u * 60u);
    assert(p.near_dry_sample_seconds == 30u * 60u);
    assert(p.critical_sample_seconds == 15u * 60u);
    assert(p.watering_sample_seconds == 2u * 60u);
    assert(p.recent_water_sample_seconds == 10u * 60u);
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
    assert(s.sample_interval_seconds == p.watering_sample_seconds);
    assert(s.has_watered);
    assert(s.seconds_since_watering == 0);

    soil_sample_t settled = sample(1800, 1450, 5);
    settled.elapsed_seconds = p.watering_sample_seconds;
    soil_model_step(&p, &settled, &s);
    assert(s.mode == SOIL_MODE_RECENTLY_WATERED);
    assert(s.seconds_since_watering == p.watering_sample_seconds);
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
    assert(s.should_report);
}

static void test_elapsed_time_drives_heartbeat(void)
{
    soil_policy_t p = soil_policy_default();
    soil_state_t s = {0};
    soil_sample_t first = sample(2000, 1450, 5);
    soil_model_step(&p, &first, &s);

    soil_sample_t unchanged = sample(2000, 1450, 5);
    unchanged.elapsed_seconds = p.heartbeat_seconds;
    soil_model_step(&p, &unchanged, &s);
    assert((s.event_flags & SOIL_EVENT_HEARTBEAT) != 0);
    assert(s.should_report);
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
    assert((s.event_flags & SOIL_EVENT_FAULT) != 0);
}

static void test_noisy_finite_sample_updates_with_zero_confidence(void)
{
    soil_policy_t p = soil_policy_default();
    soil_state_t s = {0};
    soil_sample_t first = sample(2300, 1450, 5);
    soil_model_step(&p, &first, &s);

    soil_sample_t noisy = sample(1800, 1450, p.noise_fault_mv + 1.0f);
    noisy.manual_sample = true;
    soil_model_step(&p, &noisy, &s);
    assert(s.sensor_fault);
    assert(s.current_sample_valid);
    assert(s.moisture_pct > s.previous_moisture_pct);
    assert(s.confidence_pct == 0.0f);
    assert(s.should_report);
}

static void test_manual_sample_does_not_invent_elapsed_drying_rate(void)
{
    soil_policy_t p = soil_policy_default();
    soil_state_t s = {0};
    soil_sample_t first = sample(1800, 1450, 5);
    soil_model_step(&p, &first, &s);

    soil_sample_t manual = sample(1900, 1450, 5);
    manual.manual_sample = true;
    manual.elapsed_seconds = 0;
    soil_model_step(&p, &manual, &s);
    assert(fabsf(s.drying_rate_pct_per_hour) < 0.001f);
}

static void test_telemetry_flags_expose_validity_and_history(void)
{
    soil_state_t s = {
        .event_flags = SOIL_EVENT_MANUAL | SOIL_EVENT_FAULT,
        .current_sample_valid = true,
        .has_valid_moisture = true,
        .has_watered = false,
    };

    uint16_t flags = soil_telemetry_flags(&s);
    assert((flags & SOIL_EVENT_MANUAL) != 0);
    assert((flags & SOIL_EVENT_FAULT) != 0);
    assert((flags & SOIL_TELEMETRY_STATUS_CURRENT_SAMPLE_VALID) != 0);
    assert((flags & SOIL_TELEMETRY_STATUS_HAS_VALID_MOISTURE) != 0);
    assert((flags & SOIL_TELEMETRY_STATUS_HAS_WATERED) == 0);

    s.current_sample_valid = false;
    s.has_watered = true;
    flags = soil_telemetry_flags(&s);
    assert((flags & SOIL_TELEMETRY_STATUS_CURRENT_SAMPLE_VALID) == 0);
    assert((flags & SOIL_TELEMETRY_STATUS_HAS_WATERED) != 0);
}

int main(void)
{
    test_calibration();
    test_power_policy_defaults();
    test_watering_detection();
    test_low_battery_survival();
    test_usb_without_battery_does_not_force_survival();
    test_elapsed_time_drives_heartbeat();
    test_hard_fault_preserves_last_valid_moisture();
    test_noisy_finite_sample_updates_with_zero_confidence();
    test_manual_sample_does_not_invent_elapsed_drying_rate();
    test_telemetry_flags_expose_validity_and_history();
    puts("soil_model tests passed");
    return 0;
}
