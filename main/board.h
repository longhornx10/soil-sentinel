#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    float soil_mv;
    float battery_mv;
    float noise_mv;
} board_measurement_t;

esp_err_t board_init(void);
esp_err_t board_measure(board_measurement_t *out);
esp_err_t board_read_battery_mv(float *battery_mv);
void board_prepare_sleep(void);
bool board_button_pressed(void);
uint32_t board_measure_button_hold_ms(uint32_t maximum_ms);
void board_led_status(float moisture_pct, bool diagnostic_fault, bool manual);

void board_indicator_ota_ready(void);
void board_indicator_ota_waiting(void);
void board_indicator_ota_progress(uint8_t percent);
void board_indicator_ota_success(void);
void board_indicator_ota_failure(void);
void board_indicator_ota_low_battery(void);
void board_indicator_identify(void);
void board_indicator_factory_reset_warning(void);
void board_indicator_factory_reset_confirmed(void);

esp_err_t board_pairing_indicator_start(void);
bool board_pairing_indicator_is_searching(void);
bool board_pairing_indicator_is_success(void);
void board_pairing_indicator_success(void);
void board_pairing_indicator_failure(void);
void board_pairing_indicator_off(void);
