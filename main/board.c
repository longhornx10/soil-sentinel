#include "board.h"

#include <math.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"
#include "esp_rom_sys.h"

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
#define MANUAL_LED_PULSE_US            300000U
#define SAMPLE_COUNT                   24

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_soil_cali;
static adc_cali_handle_t s_battery_cali;

static int compare_int(const void *a, const void *b)
{
    return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b);
}

static float read_channel_mv(adc_channel_t channel, adc_cali_handle_t cali, float *noise_mv)
{
    int values[SAMPLE_COUNT];
    for (size_t i = 0; i < SAMPLE_COUNT; ++i) {
        adc_oneshot_read(s_adc, channel, &values[i]);
        esp_rom_delay_us(300);
    }
    qsort(values, SAMPLE_COUNT, sizeof(values[0]), compare_int);
    long sum = 0;
    for (size_t i = 3; i < SAMPLE_COUNT - 3; ++i) sum += values[i];
    const int raw = (int)(sum / (SAMPLE_COUNT - 6));
    int mv = 0;
    if (cali && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) {
        if (noise_mv) {
            int lo = 0, hi = 0;
            adc_cali_raw_to_voltage(cali, values[3], &lo);
            adc_cali_raw_to_voltage(cali, values[SAMPLE_COUNT - 4], &hi);
            *noise_mv = (float)(hi - lo);
        }
        return (float)mv;
    }
    if (noise_mv) *noise_mv = (float)(values[SAMPLE_COUNT - 4] - values[3]);
    return raw * 3300.0f / 4095.0f;
}

esp_err_t board_init(void)
{
    /* Establish harmless levels before LEDC or status logic claims these pins. */
    gpio_config_t safe_outputs = {
        .pin_bit_mask = (1ULL << PIN_PROBE_POWER) |
                        (1ULL << PIN_LED_YELLOW) |
                        (1ULL << PIN_LED_GREEN) |
                        (1ULL << PIN_LED_RED),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&safe_outputs), TAG, "safe output GPIO init failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_PROBE_POWER, 0), TAG, "failed to hold probe output low");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_LED_YELLOW, 0), TAG, "failed to hold yellow LED low");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_LED_GREEN, 0), TAG, "failed to hold green LED low");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_LED_RED, 0), TAG, "failed to hold red LED low");

    /* Seeed Soil Moisture Sensor uses the XIAO external 2.4 GHz antenna. */
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
    ESP_LOGI(TAG, "RF switch configured for external antenna");

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
    adc_oneshot_chan_cfg_t chan_cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &chan_cfg), TAG, "battery ADC config failed");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_1, &chan_cfg), TAG, "soil ADC config failed");

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

esp_err_t board_measure(board_measurement_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "measurement output is null");
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 174);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    esp_rom_delay_us(2500);
    out->soil_mv = read_channel_mv(ADC_CHANNEL_1, s_soil_cali, &out->noise_mv);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    esp_rom_delay_us(1000);
    out->battery_mv = read_channel_mv(ADC_CHANNEL_0, s_battery_cali, NULL);
    return ESP_OK;
}

bool board_button_pressed(void)
{
    return gpio_get_level(PIN_BUTTON) == 0;
}

void board_led_status(float moisture_pct, bool fault, bool manual)
{
    gpio_set_level(PIN_LED_RED, 0);
    gpio_set_level(PIN_LED_YELLOW, 0);
    gpio_set_level(PIN_LED_GREEN, 0);
    if (!manual) return;
    gpio_num_t pin = fault ? PIN_LED_RED : moisture_pct < 20.0f ? PIN_LED_RED : moisture_pct < 30.0f ? PIN_LED_YELLOW : PIN_LED_GREEN;
    gpio_set_level(pin, 1);
    esp_rom_delay_us(MANUAL_LED_PULSE_US);
    gpio_set_level(pin, 0);
}
