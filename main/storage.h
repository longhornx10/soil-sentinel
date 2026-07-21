#pragma once

#include "esp_err.h"
#include "soil_model.h"

esp_err_t storage_init(void);
esp_err_t storage_load(soil_policy_t *policy, soil_state_t *state);
void storage_save_runtime(const soil_state_t *state);
esp_err_t storage_save_policy(const soil_policy_t *policy);
esp_err_t storage_save_checkpoint(const soil_state_t *state);
