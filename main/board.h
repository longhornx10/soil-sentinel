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
void board_led_status(float moisture_pct, bool fault, bool manual);
