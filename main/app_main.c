#include <inttypes.h>
#include <math.h>
#include "board.h"
#include "firmware_update.h"
#include "storage.h"
#include "zigbee_transport.h"
#include "soil_model.h"
#include "soil_service.h"
#include "soil_telemetry.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_private/esp_clk.h"
#include "esp_system.h"

#define NORMAL_ZIGBEE_WAIT_MS 20000U
#define COMMISSIONING_WINDOW_MS 120000U
#define REPORT_RETRY_DELAY_MS 500U
#define REPORT_SETTLE_MS 250U
#define PAIRING_RESULT_LED_MS 1200U
#define USB_NO_BATTERY_THRESHOLD_MV 500.0f
#define USB_BUTTON_POLL_MS 20U
#define USB_BUTTON_DEBOUNCE_MS 30U
#define USB_BATTERY_RECHECK_MS 1000U
#define CONFIG_DELIVERY_WINDOW_MS 5000U
#define OTA_SERVICE_WINDOW_MS (15U * 60U * 1000U)
#define OTA_WAIT_INDICATOR_MS 30000U
#define BUTTON_OTA_HOLD_MS 3000U
#define BUTTON_RESET_WARNING_MS 15000U
#define BUTTON_FACTORY_RESET_MS 20000U
#define CHECKPOINT_EVENT_MASK (SOIL_EVENT_THRESHOLD | SOIL_EVENT_WATERING | \
                               SOIL_EVENT_FAULT | SOIL_EVENT_HEARTBEAT | \
                               SOIL_EVENT_BATTERY | SOIL_EVENT_CONFIG)

typedef soil_service_button_action_t button_action_t;
#define BUTTON_ACTION_NONE          SOIL_SERVICE_BUTTON_NONE
#define BUTTON_ACTION_SAMPLE        SOIL_SERVICE_BUTTON_SAMPLE
#define BUTTON_ACTION_OTA           SOIL_SERVICE_BUTTON_OTA
#define BUTTON_ACTION_OTA_REFUSED   SOIL_SERVICE_BUTTON_OTA_REFUSED
#define BUTTON_ACTION_FACTORY_RESET SOIL_SERVICE_BUTTON_FACTORY_RESET

static const char *TAG = "soil-sentinel";

static uint32_t elapsed_seconds_between(uint64_t now_us, uint64_t before_us)
{
    if (before_us == 0U || now_us <= before_us) return 0U;
    const uint64_t elapsed = (now_us - before_us) / 1000000ULL;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static void enter_sleep(uint32_t seconds)
{
    board_pairing_indicator_off();
    board_prepare_sleep();
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_2, ESP_EXT1_WAKEUP_ANY_LOW);
    ESP_LOGI(TAG, "sleeping for %" PRIu32 " seconds", seconds);
    esp_deep_sleep_start();
}

static button_action_t classify_button_hold(float battery_mv, bool battery_present)
{
    if (!board_button_pressed()) return BUTTON_ACTION_NONE;
    vTaskDelay(pdMS_TO_TICKS(USB_BUTTON_DEBOUNCE_MS));
    if (!board_button_pressed()) return BUTTON_ACTION_NONE;

    uint32_t held_ms = USB_BUTTON_DEBOUNCE_MS;
    bool ota_announced = false;
    bool reset_warning = false;
    const bool ota_safe = !battery_present || battery_mv >= SOIL_OTA_MIN_BATTERY_MV;

    while (board_button_pressed() && held_ms < BUTTON_FACTORY_RESET_MS) {
        vTaskDelay(pdMS_TO_TICKS(USB_BUTTON_POLL_MS));
        held_ms += USB_BUTTON_POLL_MS;
        if (!ota_announced && held_ms >= BUTTON_OTA_HOLD_MS) {
            ota_announced = true;
            if (ota_safe) {
                board_indicator_ota_ready();
            } else {
                board_indicator_ota_low_battery();
            }
        }
        if (!reset_warning && held_ms >= BUTTON_RESET_WARNING_MS) {
            reset_warning = true;
            board_indicator_factory_reset_warning();
        }
    }

    while (board_button_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(USB_BUTTON_POLL_MS));
        held_ms += USB_BUTTON_POLL_MS;
        if (held_ms >= BUTTON_FACTORY_RESET_MS) {
            board_indicator_factory_reset_confirmed();
            while (board_button_pressed()) {
                vTaskDelay(pdMS_TO_TICKS(USB_BUTTON_POLL_MS));
            }
            return BUTTON_ACTION_FACTORY_RESET;
        }
    }

    return soil_service_classify_button(
        true, held_ms, battery_present, battery_mv, SOIL_OTA_MIN_BATTERY_MV,
        BUTTON_OTA_HOLD_MS, BUTTON_FACTORY_RESET_MS);
}

static void log_sample(const soil_policy_t *policy,
                       const soil_state_t *state,
                       const board_measurement_t *measurement)
{
    ESP_LOGI(TAG,
             "moisture=%.1f%% battery=%s%.1f%% raw=%.0fmV noise=%.1fmV confidence=%.0f%% "
             "mode=%s calibration=%s curve=%s interval=%" PRIu32 "s events=0x%02" PRIx32,
             state->moisture_pct,
             state->battery_present ? "" : "n/a ",
             state->battery_pct,
             measurement->soil_mv,
             measurement->noise_mv,
             state->confidence_pct,
             soil_mode_name(state->mode),
             soil_calibration_mode_name(policy->calibration_mode),
             soil_curve_source_name(state->active_curve_source),
             state->sample_interval_seconds,
             state->event_flags);
}

static esp_err_t publish_with_retry(const soil_state_t *state,
                                    const board_measurement_t *measurement)
{
    soil_diagnostics_t diag = {
        .raw_mv = measurement->soil_mv,
        .battery_mv = measurement->battery_mv,
        .noise_mv = measurement->noise_mv,
    };
    esp_err_t err = zigbee_transport_publish(state, &diag);
    if (err != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(REPORT_RETRY_DELAY_MS));
        err = zigbee_transport_publish(state, &diag);
    }
    return err;
}

static void save_state(const soil_policy_t *policy,
                       soil_state_t *state,
                       esp_err_t publish_err)
{
    if (state->should_report && publish_err != ESP_OK) {
        state->seconds_since_report = policy->heartbeat_seconds;
    }
    storage_save_runtime(state);
    if ((state->event_flags & CHECKPOINT_EVENT_MASK) != 0U) {
        (void)storage_save_checkpoint(policy, state);
    }
}

static void refresh_current_moisture(const soil_policy_t *policy,
                                     soil_state_t *state,
                                     const board_measurement_t *measurement)
{
    soil_resolve_active_curve(policy, state);
    const float value = soil_calibrated_percent(measurement->soil_mv,
                                                state->active_dry_raw_mv,
                                                state->active_wet_raw_mv);
    if (isfinite(value)) {
        state->previous_moisture_pct = state->moisture_pct;
        state->moisture_pct = value;
        state->last_reported_pct = value;
    }
}

static esp_err_t service_queued_configuration(soil_policy_t *policy,
                                               soil_state_t *state,
                                               board_measurement_t *measurement)
{
    esp_err_t final_err = ESP_OK;
    if (zigbee_transport_wait_config_change(CONFIG_DELIVERY_WINDOW_MS)) {
        refresh_current_moisture(policy, state, measurement);
        final_err = publish_with_retry(state, measurement);
        (void)storage_save_checkpoint(policy, state);
    }
    if (zigbee_transport_take_identify_request()) {
        board_indicator_identify();
    }
    return final_err;
}

static void run_ota_service_window(soil_policy_t *policy,
                                   soil_state_t *state,
                                   board_measurement_t *measurement)
{
    firmware_update_arm();
    if (zigbee_transport_begin_ota_query() != ESP_OK) {
        firmware_update_timeout();
        board_indicator_ota_failure();
        return;
    }

    uint32_t elapsed_ms = 0U;
    uint32_t indicator_ms = 0U;
    while (elapsed_ms < OTA_SERVICE_WINDOW_MS) {
        vTaskDelay(pdMS_TO_TICKS(250U));
        elapsed_ms += 250U;
        indicator_ms += 250U;

        if (zigbee_transport_wait_config_change(0U)) {
            refresh_current_moisture(policy, state, measurement);
            (void)publish_with_retry(state, measurement);
            (void)storage_save_checkpoint(policy, state);
        }
        if (zigbee_transport_take_identify_request()) board_indicator_identify();

        const soil_ota_state_t ota_state = firmware_update_state();
        if (ota_state == SOIL_OTA_STATE_FAILED ||
            ota_state == SOIL_OTA_STATE_REFUSED) {
            return;
        }
        if (indicator_ms >= OTA_WAIT_INDICATOR_MS &&
            ota_state != SOIL_OTA_STATE_DOWNLOADING) {
            board_indicator_ota_waiting();
            indicator_ms = 0U;
        }
    }

    firmware_update_timeout();
    board_indicator_ota_failure();
}

static void stay_awake_for_usb(soil_policy_t *policy,
                               soil_state_t *state,
                               bool zigbee_ready)
{
    ESP_LOGI(TAG, "USB bench mode: no battery detected; staying awake");
    uint32_t battery_recheck_ms = 0U;
    while (true) {
        if (!board_button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(USB_BUTTON_POLL_MS));
            battery_recheck_ms += USB_BUTTON_POLL_MS;
            if (battery_recheck_ms >= USB_BATTERY_RECHECK_MS) {
                float battery_mv = 0.0f;
                battery_recheck_ms = 0U;
                if (board_read_battery_mv(&battery_mv) == ESP_OK &&
                    battery_mv >= USB_NO_BATTERY_THRESHOLD_MV) {
                    ESP_LOGI(TAG, "AA battery detected in USB bench mode; restarting into battery mode");
                    esp_restart();
                }
            }
            if (zigbee_ready && zigbee_transport_wait_config_change(0U)) {
                board_measurement_t current;
                if (board_measure(&current) == ESP_OK) {
                    refresh_current_moisture(policy, state, &current);
                    (void)publish_with_retry(state, &current);
                    (void)storage_save_checkpoint(policy, state);
                }
            }
            if (zigbee_transport_take_identify_request()) board_indicator_identify();
            continue;
        }

        battery_recheck_ms = 0U;
        const button_action_t action = classify_button_hold(0.0f, false);
        if (action == BUTTON_ACTION_FACTORY_RESET) {
            (void)storage_factory_reset(true);
            esp_restart();
        }
        if (action != BUTTON_ACTION_SAMPLE) continue;

        board_measurement_t measurement;
        if (board_measure(&measurement) != ESP_OK) continue;
        const uint64_t now_us = esp_clk_rtc_time();
        soil_sample_t sample = {
            .raw_mv = measurement.soil_mv,
            .battery_mv = measurement.battery_mv,
            .noise_mv = measurement.noise_mv,
            .elapsed_seconds = elapsed_seconds_between(now_us, state->last_sample_rtc_us),
            .manual_sample = true,
            .battery_present = false,
        };
        soil_model_step(policy, &sample, state);
        log_sample(policy, state, &measurement);
        state->last_sample_rtc_us = now_us;
        board_led_status(state->current_sample_valid ? state->moisture_pct : NAN,
                         state->sensor_fault, true);
        if (!zigbee_ready) {
            zigbee_ready = zigbee_transport_wait_ready(COMMISSIONING_WINDOW_MS);
        }
        if (zigbee_ready) {
            (void)publish_with_retry(state, &measurement);
            (void)service_queued_configuration(policy, state, &measurement);
        }
        save_state(policy, state, ESP_OK);
    }
}

void app_main(void)
{
    const esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    const bool deep_sleep_wake = wake_cause == ESP_SLEEP_WAKEUP_TIMER ||
                                 wake_cause == ESP_SLEEP_WAKEUP_EXT1;
    const bool button_wake = wake_cause == ESP_SLEEP_WAKEUP_EXT1;
    const bool cold_boot = !deep_sleep_wake;
    if (deep_sleep_wake) esp_log_level_set("*", ESP_LOG_WARN);

    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(firmware_update_init());

    soil_policy_t policy;
    soil_state_t state;
    ESP_ERROR_CHECK(storage_load(&policy, &state));

    float early_battery_mv = 0.0f;
    ESP_ERROR_CHECK(board_read_battery_mv(&early_battery_mv));
    const bool early_battery_present = early_battery_mv >= USB_NO_BATTERY_THRESHOLD_MV;
    const bool button_pressed_now = board_button_pressed();
    button_action_t button_action = BUTTON_ACTION_NONE;
    if (button_wake && !button_pressed_now) {
        /* A quick press can be released before app_main runs; the wake cause is authoritative. */
        button_action = BUTTON_ACTION_SAMPLE;
    } else if (button_wake || button_pressed_now) {
        button_action = classify_button_hold(early_battery_mv, early_battery_present);
    }

    if (button_action == BUTTON_ACTION_FACTORY_RESET) {
        ESP_ERROR_CHECK(storage_factory_reset(true));
        esp_restart();
    }
    if (button_action == BUTTON_ACTION_OTA_REFUSED) {
        firmware_update_refuse_low_battery();
        enter_sleep(state.sample_interval_seconds ? state.sample_interval_seconds
                                                  : policy.stable_sample_seconds);
    }

    board_measurement_t measurement;
    ESP_ERROR_CHECK(board_measure(&measurement));
    const bool usb_without_battery = measurement.battery_mv < USB_NO_BATTERY_THRESHOLD_MV;
    esp_log_level_set("*", usb_without_battery ? ESP_LOG_INFO : ESP_LOG_WARN);

    const uint64_t sample_rtc_us = esp_clk_rtc_time();
    uint32_t elapsed_seconds = deep_sleep_wake
                                   ? elapsed_seconds_between(sample_rtc_us,
                                                             state.last_sample_rtc_us)
                                   : 0U;
    if (wake_cause == ESP_SLEEP_WAKEUP_TIMER && elapsed_seconds == 0U) {
        elapsed_seconds = state.sample_interval_seconds;
    }

    const bool manual = button_action == BUTTON_ACTION_SAMPLE;
    const bool ota_mode = button_action == BUTTON_ACTION_OTA;
    soil_sample_t sample = {
        .raw_mv = measurement.soil_mv,
        .battery_mv = measurement.battery_mv,
        .noise_mv = measurement.noise_mv,
        .elapsed_seconds = elapsed_seconds,
        .manual_sample = manual,
        .battery_present = !usb_without_battery,
    };
    soil_model_step(&policy, &sample, &state);
    if (!usb_without_battery && cold_boot) {
        /* Re-establish coordinator contact after battery insertion or any full power cycle. */
        state.event_flags |= SOIL_EVENT_HEARTBEAT;
        state.should_report = true;
        state.seconds_since_report = 0U;
    }
    state.last_sample_rtc_us = sample_rtc_us;
    board_led_status(state.current_sample_valid ? state.moisture_pct : NAN,
                     state.sensor_fault, manual);
    log_sample(&policy, &state, &measurement);

    soil_diagnostics_t diag = {
        .raw_mv = measurement.soil_mv,
        .battery_mv = measurement.battery_mv,
        .noise_mv = measurement.noise_mv,
    };
    const bool validation_boot = firmware_update_needs_boot_validation();
    const bool need_zigbee = state.should_report || usb_without_battery ||
                              ota_mode || validation_boot;
    bool zigbee_ready = false;
    esp_err_t publish_err = ESP_OK;

    if (need_zigbee) {
        ESP_ERROR_CHECK(zigbee_transport_start(&policy, &state, &diag, ota_mode));
        const uint32_t wait_ms = (manual || usb_without_battery || ota_mode ||
                                  validation_boot || cold_boot)
                                     ? COMMISSIONING_WINDOW_MS
                                     : NORMAL_ZIGBEE_WAIT_MS;
        zigbee_ready = zigbee_transport_wait_ready(wait_ms);
        if (zigbee_ready) {
            const bool paired_now = board_pairing_indicator_is_success();
            publish_err = publish_with_retry(&state, &measurement);
            if (publish_err == ESP_OK) {
                const esp_err_t config_err = service_queued_configuration(
                    &policy, &state, &measurement);
                if (config_err != ESP_OK) publish_err = config_err;
            }
            if (validation_boot && publish_err == ESP_OK) {
                ESP_ERROR_CHECK(firmware_update_mark_running_valid());
                /* Publish the accepted slot/result immediately instead of leaving HA
                 * with a stale "validation pending" diagnostic until tomorrow. */
                publish_err = publish_with_retry(&state, &measurement);
            }
            vTaskDelay(pdMS_TO_TICKS(paired_now ? PAIRING_RESULT_LED_MS
                                                : REPORT_SETTLE_MS));
            board_pairing_indicator_off();

            if (ota_mode) {
                float loaded_battery_mv = measurement.battery_mv;
                const bool loaded_battery_safe =
                    !state.battery_present ||
                    (board_read_battery_mv(&loaded_battery_mv) == ESP_OK &&
                     loaded_battery_mv >= SOIL_OTA_MIN_BATTERY_MV);
                if (!loaded_battery_safe) {
                    firmware_update_refuse_low_battery();
                    board_indicator_ota_low_battery();
                } else {
                    measurement.battery_mv = loaded_battery_mv;
                    run_ota_service_window(&policy, &state, &measurement);
                }
            }
        } else {
            publish_err = ESP_ERR_TIMEOUT;
            if (!usb_without_battery && state.sample_interval_seconds < 3600U) {
                state.sample_interval_seconds = 3600U;
            }
        }
    }

    save_state(&policy, &state, publish_err);
    if (usb_without_battery) stay_awake_for_usb(&policy, &state, zigbee_ready);
    enter_sleep(state.sample_interval_seconds ? state.sample_interval_seconds
                                              : policy.stable_sample_seconds);
}
