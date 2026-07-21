#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "soil_model.h"

static void test_calibration(void)
{
    assert(fabsf(soil_calibrated_percent(2750, 2750, 1200) - 0.0f) < 0.01f);
    assert(fabsf(soil_calibrated_percent(1200, 2750, 1200) - 100.0f) < 0.01f);
    assert(isnan(soil_calibrated_percent(2000, 2000, 1950)));
}

static void test_watering_detection(void)
{
    soil_policy_t p = soil_policy_default();
    soil_state_t s = {0};
    soil_sample_t first = {.raw_mv = 2300, .battery_mv = 1450, .noise_mv = 5};
    soil_model_step(&p, &first, &s);
    s.sample_interval_seconds = 900;
    soil_sample_t watered = {.raw_mv = 1800, .battery_mv = 1450, .noise_mv = 5};
    soil_model_step(&p, &watered, &s);
    assert(s.mode == SOIL_MODE_WATERING_CAPTURE);
    assert((s.event_flags & SOIL_EVENT_WATERING) != 0);
}

static void test_low_battery_survival(void)
{
    soil_policy_t p = soil_policy_default();
    soil_state_t s = {0};
    soil_sample_t first = {.raw_mv = 2000, .battery_mv = 1050, .noise_mv = 5};
    soil_model_step(&p, &first, &s);
    soil_model_step(&p, &first, &s);
    assert(s.mode == SOIL_MODE_SURVIVAL);
    assert(s.sample_interval_seconds == 12u * 60u * 60u);
}

int main(void)
{
    test_calibration();
    test_watering_detection();
    test_low_battery_survival();
    puts("soil_model tests passed");
    return 0;
}
