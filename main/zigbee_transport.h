#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "soil_model.h"

typedef struct {
    float raw_mv;
    float battery_mv;
    float noise_mv;
} soil_diagnostics_t;

esp_err_t zigbee_transport_start(soil_policy_t *policy,
                                 soil_state_t *state,
                                 soil_diagnostics_t *diagnostics,
                                 bool ota_mode);
bool zigbee_transport_wait_ready(uint32_t timeout_ms);
esp_err_t zigbee_transport_publish(const soil_state_t *state,
                                   const soil_diagnostics_t *diagnostics);
bool zigbee_transport_wait_config_change(uint32_t timeout_ms);
bool zigbee_transport_take_identify_request(void);
esp_err_t zigbee_transport_begin_ota_query(void);
