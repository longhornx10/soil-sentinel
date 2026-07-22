#include "board.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define PIN_BATTERY          GPIO_NUM_0
#define PIN_SOIL             GPIO_NUM_1
#define PIN_BUTTON           GPIO_NUM_2
#define PIN_RF_SWITCH_ENABLE GPIO_NUM_3
#define PIN_RF_ANT_SELECT    GPIO_NUM_14
#define PIN_PROBE_POWER      GPIO_NUM_21
#define PIN_LED_YELLOW       GPIO_NUM_18
#define PIN_LED_GREEN        GPIO_NUM_19
#define PIN_LED_RED          GPIO_NUM_20

#define RF_SWITCH_CONTROL_ENABLE_LEVEL 0
#define RF_EXTERNAL_ANTENNA_LEVEL      1
#define MANUAL_LED_PULSE_MS             300U
#define SHORT_BLINK_MS                  110U
#define STATUS_GAP_MS                   90U
#define PAIRING_BLINK_INTERVAL_MS       300U
#define SAMPLE_COUNT                    24U
#define SOIL_SETTLE_MS                  1000U
#define SOIL_READING_COUNT              10U
#define SOIL_READING_INTERVAL_MS        200U
#define PROBE_PWM_DUTY                   87U
#define LED_DRY_MAX_PCT                 20.0f
#define LED_MOIST_MAX_PCT               60.0f
#define BUTTON_POLL_MS                  20U
#define BUTTON_DEBOUNCE_MS              30U

typedef enum {
    PAIRING_LED_OFF = 0,
    PAIRING_LED_SEARCHING,
    PAIRING_LED_SUCCESS,
    PAIRING_LED_FAILURE,
} pairing_led_state_t;

static const gpio_num_t s_sleep_held_outputs[] = {
    PIN_RF_SWITCH_ENABLE,
    PIN_RF_ANT_SELECT,
    PIN_PROBE_POWER,
    PIN_LED_YELLOW,
    PIN_LED_GREEN,
    PIN_LED_RED,
};

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_soil_cali;
static adc_cali_handle_t s_battery_cali;
static volatile pairing_led_state_t s_pairing_led_state = PAIRING_LED_OFF;
static TaskHandle_t s_pairing_blink_task;

static void release_sleep_output_holds(void)
{
    gpio_deep_sleep_hold_dis();
    for (size_t i = 0; i < sizeof(s_sleep_held_outputs) / sizeof(s_sleep_held_outputs[0]); ++i) {
        (void)gpio_hold_dis(s_sleep_held_outputs[i]);
    }
}

static void hold_sleep_outputs(void)
{
    for (size_t i = 0; i < sizeof(s_sleep_held_outputs) / sizeof(s_sleep_held_outputs[0]); ++i) {
        (void)gpio_hold_en(s_sleep_held_outputs[i]);
    }
    gpio_deep_sleep_hold_en();
}

static void set_leds(bool red, bool yellow, bool green)
{
    gpio_set_level(PIN_LED_RED, red ? 1 : 0);
    gpio_set_level(PIN_LED_YELLOW, yellow ? 1 : 0);
    gpio_set_level(PIN_LED_GREEN, green ? 1 : 0);
}

static void led_step(bool red, bool yellow, bool green, uint32_t on_ms, uint32_t off_ms)
{
    set_leds(red, yellow, green);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    set_leds(false, false, false);
    if (off_ms) vTaskDelay(pdMS_TO_TICKS(off_ms));
}

static void repeat_led(bool red, bool yellow, bool green, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        led_step(red, yellow, green, SHORT_BLINK_MS, i + 1U < count ? STATUS_GAP_MS : 0U);
    }
}

static esp_err_t set_probe_duty(uint32_t duty)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG,
                        "failed to set probe PWM duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void pairing_blink_task(void *arg)
{
    (void)arg;
    bool red_on = false;
    while (s_pairing_led_state == PAIRING_LED_SEARCHING) {
        red_on = !red_on;
        set_leds(red_on, false, false);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PAIRING_BLINK_INTERVAL_MS));
    }
    gpio_set_level(PIN_LED_RED, 0);
    s_pairing_blink_task = NULL;
    vTaskDelete(NULL);
}

static int compare_int(const void *a, const void *b)
{
    return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b);
}

static esp_err_t read_channel_mv(adc_channel_t channel,
                                 adc_cali_handle_t cali,
                                 float *value_mv,
                                 float *noise_mv)
{
    ESP_RETURN_ON_FALSE(value_mv, ESP_ERR_INVALID_ARG, TAG, "ADC output pointer is null");
    int values[SAMPLE_COUNT];
    for (size_t i = 0; i < SAMPLE_COUNT; ++i) {
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, channel, &values[i]), TAG,
                            "ADC oneshot read failed");
        esp_rom_delay_us(300);
    }
    qsort(values, SAMPLE_COUNT, sizeof(values[0]), compare_int);
    long sum = 0;
    for (size_t i = 3; i < SAMPLE_COUNT - 3; ++i) sum += values[i];
    const int raw = (int)(sum / (SAMPLE_COUNT - 6));

    int mv = 0;
    if (cali && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) {
        *value_mv = (float)mv;
        if (noise_mv) {
            int lo = 0, hi = 0;
            if (adc_cali_raw_to_voltage(cali, values[3], &lo) == ESP_OK &&
                adc_cali_raw_to_voltage(cali, values[SAMPLE_COUNT - 4], &hi) == ESP_OK) {
                *noise_mv = (float)(hi - lo);
            } else {
                *noise_mv = 0.0f;
            }
        }
        return ESP_OK;
    }

    *value_mv = raw * 3300.0f / 4095.0f;
    if (noise_mv) {
        *noise_mv = (values[SAMPLE_COUNT - 4] - values[3]) * 3300.0f / 4095.0f;
    }
    return ESP_OK;
}

static esp_err_t read_settled_soil_mv(float *value_mv, float *noise_mv)
{
    ESP_RETURN_ON_FALSE(value_mv && noise_mv, ESP_ERR_INVALID_ARG, TAG,
                        "soil measurement output pointer is null");
    float sum_mv = 0.0f;
    float min_mv = INFINITY;
    float max_mv = -INFINITY;
    float burst_noise_sum_mv = 0.0f;
    for (size_t i = 0; i < SOIL_READING_COUNT; ++i) {
        float sample_mv = 0.0f;
        float burst_noise_mv = 0.0f;
        ESP_RETURN_ON_ERROR(read_channel_mv(ADC_CHANNEL_1, s_soil_cali,
                                            &sample_mv, &burst_noise_mv), TAG,
                            "soil ADC read failed");
        sum_mv += sample_mv;
        burst_noise_sum_mv += burst_noise_mv;
        if (sample_mv < min_mv) min_mv = sample_mv;
        if (sample_mv > max_mv) max_mv = sample_mv;
        if (i + 1U < SOIL_READING_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(SOIL_READING_INTERVAL_MS));
        }
    }
    *noise_mv = fmaxf(max_mv - min_mv,
                      burst_noise_sum_mv / (float)SOIL_READING_COUNT);
    *value_mv = sum_mv / (float)SOIL_READING_COUNT;
    return ESP_OK;
}

esp_err_t board_init(void)
{
    release_sleep_output_holds();

    gpio_config_t safe_outputs = {
        .pin_bit_mask = (1ULL << PIN_PROBE_POWER) |
                        (1ULL << PIN_LED_YELLOW) |
                        (1ULL << PIN_LED_GREEN) |
                        (1ULL << PIN_LED_RED),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&safe_outputs), TAG, "safe output GPIO init failed");
    set_leds(false, false, false);
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_PROBE_POWER, 0), TAG, "failed to hold probe output low");

    gpio_config_t rf_enable = {
        .pin_bit_mask = 1ULL << PIN_RF_SWITCH_ENABLE,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rf_enable), TAG, "RF switch enable GPIO init failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_RF_SWITCH_ENABLE, RF_SWITCH_CONTROL_ENABLE_LEVEL), TAG,
                        "failed to enable RF switch control");
    esp_rom_delay_us(100000);

    gpio_config_t rf_select = {
        .pin_bit_mask = 1ULL << PIN_RF_ANT_SELECT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rf_select), TAG, "RF antenna select GPIO init failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_RF_ANT_SELECT, RF_EXTERNAL_ANTENNA_LEVEL), TAG,
                        "failed to select external RF antenna");

    gpio_config_t button = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&button), TAG, "button GPIO init failed");

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 200000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "probe timer init failed");
    ledc_channel_config_t channel = {
        .gpio_num = PIN_PROBE_POWER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "probe PWM init failed");

    adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = ADC_UNIT_1};
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "ADC init failed");
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &chan_cfg), TAG,
                        "battery ADC config failed");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_1, &chan_cfg), TAG,
                        "soil ADC config failed");

    adc_cali_curve_fitting_config_t soil_cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_cali_curve_fitting_config_t battery_cali_cfg = soil_cali_cfg;
    battery_cali_cfg.chan = ADC_CHANNEL_0;
    (void)adc_cali_create_scheme_curve_fitting(&soil_cali_cfg, &s_soil_cali);
    (void)adc_cali_create_scheme_curve_fitting(&battery_cali_cfg, &s_battery_cali);
    return ESP_OK;
}

esp_err_t board_read_battery_mv(float *battery_mv)
{
    return read_channel_mv(ADC_CHANNEL_0, s_battery_cali, battery_mv, NULL);
}

esp_err_t board_measure(board_measurement_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "measurement output is null");
    ESP_RETURN_ON_ERROR(set_probe_duty(PROBE_PWM_DUTY), TAG, "failed to start probe excitation");
    vTaskDelay(pdMS_TO_TICKS(SOIL_SETTLE_MS));
    const esp_err_t soil_err = read_settled_soil_mv(&out->soil_mv, &out->noise_mv);
    const esp_err_t stop_err = set_probe_duty(0);
    vTaskDelay(pdMS_TO_TICKS(1));
    if (soil_err != ESP_OK) return soil_err;
    if (stop_err != ESP_OK) return stop_err;
    return board_read_battery_mv(&out->battery_mv);
}

void board_prepare_sleep(void)
{
    (void)set_probe_duty(0);
    set_leds(false, false, false);
    (void)gpio_set_level(PIN_RF_SWITCH_ENABLE, RF_SWITCH_CONTROL_ENABLE_LEVEL);
    (void)gpio_set_level(PIN_RF_ANT_SELECT, RF_EXTERNAL_ANTENNA_LEVEL);
    hold_sleep_outputs();
}

bool board_button_pressed(void)
{
    return gpio_get_level(PIN_BUTTON) == 0;
}

uint32_t board_measure_button_hold_ms(uint32_t maximum_ms)
{
    if (!board_button_pressed()) return 0U;
    vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
    if (!board_button_pressed()) return 0U;
    uint32_t held_ms = BUTTON_DEBOUNCE_MS;
    while (board_button_pressed() && held_ms < maximum_ms) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        held_ms += BUTTON_POLL_MS;
    }
    return held_ms;
}

void board_led_status(float moisture_pct, bool diagnostic_fault, bool manual)
{
    set_leds(false, false, false);
    if (!manual) return;
    if (!isfinite(moisture_pct)) {
        repeat_led(true, false, false, 3U);
        return;
    }
    const gpio_num_t pin = moisture_pct < LED_DRY_MAX_PCT ? PIN_LED_RED
                           : moisture_pct < LED_MOIST_MAX_PCT ? PIN_LED_YELLOW
                                                              : PIN_LED_GREEN;
    gpio_set_level(pin, 1);
    vTaskDelay(pdMS_TO_TICKS(MANUAL_LED_PULSE_MS));
    gpio_set_level(pin, 0);
    if (diagnostic_fault) ESP_LOGW(TAG, "manual display completed with diagnostic fault");
}

void board_indicator_ota_ready(void)
{
    led_step(true, false, false, SHORT_BLINK_MS, STATUS_GAP_MS);
    led_step(false, true, false, SHORT_BLINK_MS, STATUS_GAP_MS);
    led_step(false, false, true, SHORT_BLINK_MS, 0U);
}

void board_indicator_ota_waiting(void) { led_step(false, true, false, 80U, 0U); }

void board_indicator_ota_progress(uint8_t percent)
{
    if (percent == 0U || percent >= 100U || (percent % 10U) != 0U) return;
    led_step(false, true, false, 45U, 0U);
}

void board_indicator_ota_success(void) { repeat_led(false, false, true, 3U); }
void board_indicator_ota_failure(void) { repeat_led(true, false, false, 3U); }
void board_indicator_ota_low_battery(void) { repeat_led(true, false, false, 3U); }

void board_indicator_identify(void)
{
    for (unsigned i = 0; i < 2U; ++i) {
        led_step(true, false, false, 100U, 60U);
        led_step(false, true, false, 100U, 60U);
        led_step(false, false, true, 100U, 60U);
    }
}

void board_indicator_factory_reset_warning(void)
{
    for (unsigned i = 0; i < 4U; ++i) {
        led_step(true, false, false, 100U, 60U);
        led_step(false, false, true, 100U, 60U);
    }
}

void board_indicator_factory_reset_confirmed(void)
{
    repeat_led(true, true, true, 3U);
}

esp_err_t board_pairing_indicator_start(void)
{
    set_leds(false, false, false);
    s_pairing_led_state = PAIRING_LED_SEARCHING;
    if (s_pairing_blink_task) {
        xTaskNotifyGive(s_pairing_blink_task);
        return ESP_OK;
    }
    if (xTaskCreate(pairing_blink_task, "pair_led", 2048, NULL, 3,
                    &s_pairing_blink_task) != pdPASS) {
        s_pairing_led_state = PAIRING_LED_OFF;
        s_pairing_blink_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool board_pairing_indicator_is_searching(void) { return s_pairing_led_state == PAIRING_LED_SEARCHING; }
bool board_pairing_indicator_is_success(void) { return s_pairing_led_state == PAIRING_LED_SUCCESS; }

void board_pairing_indicator_success(void)
{
    s_pairing_led_state = PAIRING_LED_SUCCESS;
    if (s_pairing_blink_task) xTaskNotifyGive(s_pairing_blink_task);
    set_leds(false, false, true);
}

void board_pairing_indicator_failure(void)
{
    s_pairing_led_state = PAIRING_LED_FAILURE;
    if (s_pairing_blink_task) xTaskNotifyGive(s_pairing_blink_task);
    set_leds(false, true, false);
}

void board_pairing_indicator_off(void)
{
    s_pairing_led_state = PAIRING_LED_OFF;
    if (s_pairing_blink_task) xTaskNotifyGive(s_pairing_blink_task);
    set_leds(false, false, false);
}
