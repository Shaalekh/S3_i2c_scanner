#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ads1115_write_config(i2c_master_dev_handle_t dev, uint16_t config);
esp_err_t ads1115_read_config(i2c_master_dev_handle_t dev, uint16_t *out_config);
esp_err_t ads1115_read_conversion(i2c_master_dev_handle_t dev, int16_t *out_raw);

// Initialize ADS1115 device handle if any driver-level init is required.
esp_err_t ads1115_init(i2c_master_dev_handle_t dev);

// Read single-shot from single-ended channel 0..3. Returns raw ADC value (signed 16-bit).
esp_err_t ads1115_read_single_shot(i2c_master_dev_handle_t dev, uint8_t channel, int16_t *out_raw);

#ifdef __cplusplus
}
#endif
