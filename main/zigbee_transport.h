#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "soil_model.h"

typedef struct {
    float raw_mv;
    float battery_mv;
    float noise_mv;
} soil_diagnostics_t;

esp_err_t zigbee_transport_start(void);
bool zigbee_transport_wait_ready(uint32_t timeout_ms);
esp_err_t zigbee_transport_publish(const soil_state_t *state, const soil_diagnostics_t *diag);
