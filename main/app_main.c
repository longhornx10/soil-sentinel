#include <inttypes.h>
#include <math.h>
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
#define USB_BUTTON_POLL_MS 20U
#define USB_BUTTON_DEBOUNCE_MS 30U
#define CHECKPOINT_EVENT_MASK (SOIL_EVENT_THRESHOLD | SOIL_EVENT_WATERING | SOIL_EVENT_FAULT | \
                               SOIL_EVENT_HEARTBEAT | SOIL_EVENT_BATTERY)

static const char *TAG = "soil-sentinel";

static void enter_sleep(uint32_t seconds)
{
    board_pairing_indicator_off();
    board_prepare_sleep();
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_2, ESP_EXT1_WAKEUP_ANY_LOW);
    ESP_LOGI(TAG, "sleeping for %" PRIu32 " seconds", seconds);
    esp_deep_sleep_start();
}

static void log_sample(const soil_state_t *state, const board_measurement_t *measurement)
{
    ESP_LOGI(TAG,
             "moisture=%.1f%% battery=%s%.1f%% raw=%.0fmV noise=%.1fmV confidence=%.0f%% "
             "mode=%s interval=%" PRIu32 "s events=0x%02" PRIx32 " fault=%s valid=%s report=%s",
             state->moisture_pct, state->battery_present ? "" : "n/a ", state->battery_pct,
             measurement->soil_mv, measurement->noise_mv, state->confidence_pct,
             soil_mode_name(state->mode), state->sample_interval_seconds, state->event_flags,
             state->sensor_fault ? "yes" : "no", state->current_sample_valid ? "yes" : "no",
             state->should_report ? "yes" : "no");
}

static esp_err_t publish_with_retry(const soil_state_t *state, const board_measurement_t *measurement)
{
    soil_diagnostics_t diag = {
        .raw_mv = measurement->soil_mv,
        .battery_mv = measurement->battery_mv,
        .noise_mv = measurement->noise_mv,
    };
    soil_state_t reported_state = *state;
    if (!reported_state.current_sample_valid) {
        /* ZCL single precision represents unknown as NaN. Keep last-good state internally. */
        reported_state.moisture_pct = NAN;
    }

    esp_err_t publish_err = zigbee_transport_publish(&reported_state, &diag);
    if (publish_err != ESP_OK) {
        ESP_LOGW(TAG, "Zigbee report delivery failed; retrying once in %u ms", REPORT_RETRY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(REPORT_RETRY_DELAY_MS));
        publish_err = zigbee_transport_publish(&reported_state, &diag);
    }

    if (publish_err == ESP_OK) {
        ESP_LOGI(TAG, "Zigbee state report delivery confirmed");
    } else {
        ESP_LOGE(TAG, "Zigbee state report delivery failed after retry");
    }
    return publish_err;
}

static void save_state(const soil_policy_t *policy, soil_state_t *state, esp_err_t publish_err)
{
    if (state->should_report && publish_err != ESP_OK) {
        /* Force another report attempt instead of treating this sample as delivered. */
        state->seconds_since_report = policy->heartbeat_seconds;
    }

    storage_save_runtime(state);

    /* Manual-only checks stay in RTC state and do not consume an NVS write. */
    if ((state->event_flags & CHECKPOINT_EVENT_MASK) != 0) {
        (void)storage_save_checkpoint(state);
    }
}

static void stay_awake_for_usb(const soil_policy_t *policy, soil_state_t *state, bool zigbee_ready)
{
    ESP_LOGI(TAG, "USB bench mode: no battery detected; staying awake for monitor and flashing");
    ESP_LOGI(TAG, "USB bench mode: press the large sensor button for a fresh sample and Zigbee report");
    int64_t previous_sample_us = esp_timer_get_time();

    while (true) {
        if (!board_button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(USB_BUTTON_POLL_MS));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(USB_BUTTON_DEBOUNCE_MS));
        if (!board_button_pressed()) {
            continue;
        }

        /* One action per press. Wait for release before measuring. */
        while (board_button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(USB_BUTTON_POLL_MS));
        }

        ESP_LOGI(TAG, "USB bench button pressed; taking a fresh manual sample");

        board_measurement_t measurement;
        const esp_err_t measure_err = board_measure(&measurement);
        if (measure_err != ESP_OK) {
            ESP_LOGE(TAG, "USB bench measurement failed: %s", esp_err_to_name(measure_err));
            continue;
        }

        const int64_t now_us = esp_timer_get_time();
        const uint32_t elapsed_seconds = now_us > previous_sample_us
                                                 ? (uint32_t)((now_us - previous_sample_us) / 1000000ULL)
                                                 : 0U;
        previous_sample_us = now_us;

        soil_sample_t sample = {
            .raw_mv = measurement.soil_mv,
            .battery_mv = measurement.battery_mv,
            .noise_mv = measurement.noise_mv,
            .elapsed_seconds = elapsed_seconds,
            .manual_sample = true,
            .battery_present = false,
        };
        soil_model_step(policy, &sample, state);
        board_led_status(state->current_sample_valid ? state->moisture_pct : NAN,
                         state->sensor_fault, true);
        log_sample(state, &measurement);

        if (!zigbee_ready) {
            ESP_LOGI(TAG, "USB bench mode: waiting for Zigbee before manual report");
            zigbee_ready = zigbee_transport_wait_ready(COMMISSIONING_WINDOW_MS);
        }

        esp_err_t publish_err = ESP_ERR_INVALID_STATE;
        if (zigbee_ready) {
            publish_err = publish_with_retry(state, &measurement);
            vTaskDelay(pdMS_TO_TICKS(REPORT_SETTLE_MS));
            board_pairing_indicator_off();
        } else {
            ESP_LOGW(TAG, "USB bench manual sample retained because Zigbee is not ready");
        }

        save_state(policy, state, publish_err);
        vTaskDelay(pdMS_TO_TICKS(USB_BUTTON_DEBOUNCE_MS));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(board_init());

    soil_policy_t policy;
    soil_state_t state;
    ESP_ERROR_CHECK(storage_load(&policy, &state));

    const esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    const bool timer_wake = wake_cause == ESP_SLEEP_WAKEUP_TIMER;
    const bool button_wake = wake_cause == ESP_SLEEP_WAKEUP_EXT1;
    const uint32_t elapsed_seconds = timer_wake ? state.sample_interval_seconds : 0U;

    board_measurement_t measurement;
    ESP_ERROR_CHECK(board_measure(&measurement));
    const bool manual = button_wake || board_button_pressed();
    /* The supported service workflow removes the AA before USB power is attached. */
    const bool usb_without_battery = measurement.battery_mv < USB_NO_BATTERY_THRESHOLD_MV;

    soil_sample_t sample = {
        .raw_mv = measurement.soil_mv,
        .battery_mv = measurement.battery_mv,
        .noise_mv = measurement.noise_mv,
        .elapsed_seconds = elapsed_seconds,
        .manual_sample = manual,
        .battery_present = !usb_without_battery,
    };
    soil_model_step(&policy, &sample, &state);
    board_led_status(state.current_sample_valid ? state.moisture_pct : NAN,
                     state.sensor_fault, manual);
    log_sample(&state, &measurement);

    bool zigbee_ready = false;
    esp_err_t publish_err = ESP_OK;

    if (state.should_report || usb_without_battery) {
        ESP_ERROR_CHECK(zigbee_transport_start());
        const uint32_t wait_ms = (manual || usb_without_battery) ? COMMISSIONING_WINDOW_MS : NORMAL_ZIGBEE_WAIT_MS;
        ESP_LOGI(TAG, "Zigbee commissioning/report window: %" PRIu32 " ms", wait_ms);
        zigbee_ready = zigbee_transport_wait_ready(wait_ms);

        if (zigbee_ready) {
            const bool paired_now = board_pairing_indicator_is_success();
            if (state.should_report) {
                publish_err = publish_with_retry(&state, &measurement);
            }
            vTaskDelay(pdMS_TO_TICKS(paired_now ? PAIRING_RESULT_LED_MS : REPORT_SETTLE_MS));
            board_pairing_indicator_off();
        } else {
            publish_err = ESP_ERR_TIMEOUT;
            const bool pairing_failed = board_pairing_indicator_is_searching();
            ESP_LOGW(TAG, "Zigbee unavailable after %" PRIu32 " ms; retaining state and backing off", wait_ms);
            if (pairing_failed) {
                board_pairing_indicator_failure();
                vTaskDelay(pdMS_TO_TICKS(PAIRING_RESULT_LED_MS));
                board_pairing_indicator_off();
            }
            if (!usb_without_battery && state.sample_interval_seconds < 3600U) {
                state.sample_interval_seconds = 3600U;
            }
        }
    }

    save_state(&policy, &state, publish_err);

    if (usb_without_battery) {
        stay_awake_for_usb(&policy, &state, zigbee_ready);
    }

    enter_sleep(state.sample_interval_seconds ? state.sample_interval_seconds : policy.stable_sample_seconds);
}
