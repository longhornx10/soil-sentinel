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

#define NORMAL_ZIGBEE_WAIT_MS 20000U
#define COMMISSIONING_WINDOW_MS 120000U
#define REPORT_RETRY_DELAY_MS 500U
#define REPORT_SETTLE_MS 250U
#define PAIRING_RESULT_LED_MS 1200U
#define USB_NO_BATTERY_THRESHOLD_MV 500.0f
#define USB_IDLE_DELAY_MS 60000U

static const char *TAG = "soil-sentinel";

static void enter_sleep(uint32_t seconds)
{
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_2, ESP_EXT1_WAKEUP_ANY_LOW);
    ESP_LOGI(TAG, "sleeping for %" PRIu32 " seconds", seconds);
    esp_deep_sleep_start();
}

static void stay_awake_for_usb(void)
{
    ESP_LOGI(TAG, "USB bench mode: no battery detected; staying awake for monitor and flashing");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(USB_IDLE_DELAY_MS));
    }
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
    /* The supported service workflow removes the AA before USB power is attached. */
    const bool usb_without_battery = measurement.battery_mv < USB_NO_BATTERY_THRESHOLD_MV;
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
        const uint32_t wait_ms = (manual || usb_without_battery) ? COMMISSIONING_WINDOW_MS : NORMAL_ZIGBEE_WAIT_MS;
        ESP_LOGI(TAG, "Zigbee commissioning/report window: %" PRIu32 " ms", wait_ms);
        if (zigbee_transport_wait_ready(wait_ms)) {
            const bool paired_now = board_pairing_indicator_is_success();
            soil_diagnostics_t diag = {
                .raw_mv = measurement.soil_mv,
                .battery_mv = measurement.battery_mv,
                .noise_mv = measurement.noise_mv,
            };

            esp_err_t publish_err = zigbee_transport_publish(&state, &diag);
            if (publish_err != ESP_OK) {
                ESP_LOGW(TAG, "Zigbee report delivery failed; retrying once in %u ms", REPORT_RETRY_DELAY_MS);
                vTaskDelay(pdMS_TO_TICKS(REPORT_RETRY_DELAY_MS));
                publish_err = zigbee_transport_publish(&state, &diag);
            }

            if (publish_err == ESP_OK) {
                ESP_LOGI(TAG, "Zigbee moisture report delivery confirmed");
            } else {
                ESP_LOGE(TAG, "Zigbee moisture report delivery failed after retry");
                /* Force the next wake to attempt another report instead of treating this sample as delivered. */
                state.seconds_since_report = policy.heartbeat_seconds;
            }

            vTaskDelay(pdMS_TO_TICKS(paired_now ? PAIRING_RESULT_LED_MS : REPORT_SETTLE_MS));
            board_pairing_indicator_off();
        } else {
            const bool pairing_failed = board_pairing_indicator_is_searching();
            ESP_LOGW(TAG, "Zigbee unavailable after %" PRIu32 " ms; retaining state and backing off", wait_ms);
            if (pairing_failed) {
                board_pairing_indicator_failure();
                vTaskDelay(pdMS_TO_TICKS(PAIRING_RESULT_LED_MS));
                board_pairing_indicator_off();
            }
            if (state.sample_interval_seconds < 3600) state.sample_interval_seconds = 3600;
            state.seconds_since_report = policy.heartbeat_seconds;
        }
    }

    /* RTC memory retains every wake; flash checkpoints remain event-driven. */
    storage_save_runtime(&state);
    if (state.should_report || manual) (void)storage_save_checkpoint(&state);

    if (usb_without_battery) {
        stay_awake_for_usb();
    }

    enter_sleep(state.sample_interval_seconds ? state.sample_interval_seconds : policy.stable_sample_seconds);
}
