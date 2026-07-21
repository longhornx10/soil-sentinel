#include <inttypes.h>
#include "board.h"
#include "storage.h"
#include "zigbee_transport.h"
#include "soil_model.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"

static const char *TAG = "soil-sentinel";

static void enter_sleep(uint32_t seconds)
{
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_2, ESP_EXT1_WAKEUP_ANY_LOW);
    ESP_LOGI(TAG, "sleeping for %" PRIu32 " seconds", seconds);
    esp_deep_sleep_start();
}

void app_main(void)
{
    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(board_init());

    soil_policy_t policy;
    soil_state_t state;
    ESP_ERROR_CHECK(storage_load(&policy, &state));

    board_measurement_t measurement;
    ESP_ERROR_CHECK(board_measure(&measurement));
    const bool manual = board_button_pressed() || esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1;
    soil_sample_t sample = {
        .raw_mv = measurement.soil_mv,
        .battery_mv = measurement.battery_mv,
        .noise_mv = measurement.noise_mv,
        .uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000ULL),
        .manual_sample = manual,
    };
    soil_model_step(&policy, &sample, &state);
    board_led_status(state.moisture_pct, state.sensor_fault, manual);

    ESP_LOGI(TAG, "moisture=%.1f%% battery=%.1f%% raw=%.0fmV noise=%.1fmV mode=%s report=%s",
             state.moisture_pct, state.battery_pct, measurement.soil_mv, measurement.noise_mv,
             soil_mode_name(state.mode), state.should_report ? "yes" : "no");

    if (state.should_report) {
        ESP_ERROR_CHECK(zigbee_transport_start());
        if (zigbee_transport_wait_ready(15000)) {
            soil_diagnostics_t diag = {
                .raw_mv = measurement.soil_mv,
                .battery_mv = measurement.battery_mv,
                .noise_mv = measurement.noise_mv,
            };
            (void)zigbee_transport_publish(&state, &diag);
            vTaskDelay(pdMS_TO_TICKS(750));
        } else {
            ESP_LOGW(TAG, "Zigbee unavailable; retaining state and backing off");
            if (state.sample_interval_seconds < 3600) state.sample_interval_seconds = 3600;
        }
    }

    /* Checkpoint only on report/event wakes to avoid routine flash writes. */
    if (state.should_report || manual) (void)storage_save_checkpoint(&state);
    enter_sleep(state.sample_interval_seconds ? state.sample_interval_seconds : policy.stable_sample_seconds);
}
