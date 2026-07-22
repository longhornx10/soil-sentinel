#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    float soil_mv;
    float battery_mv;
    float noise_mv;
} board_measurement_t;

esp_err_t board_init(void);
esp_err_t board_measure(board_measurement_t *out);
bool board_button_pressed(void);
void board_led_status(float moisture_pct, bool sample_valid, bool manual);

esp_err_t board_pairing_indicator_start(void);
bool board_pairing_indicator_is_searching(void);
bool board_pairing_indicator_is_success(void);
void board_pairing_indicator_success(void);
void board_pairing_indicator_failure(void);
void board_pairing_indicator_off(void);
