#pragma once

#include "esp_err.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize filesystem for logging (SPIFFS)
esp_err_t data_logger_init(void);

// Deinitialize / unmount filesystem
esp_err_t data_logger_deinit(void);

// Create a unique CSV file and return FILE*; out_name will contain the created filename
FILE *data_logger_create_unique_file(char *out_name, size_t len);

// Write a CSV row (timestamp in ms, eeg, ecg, gsr, temp_c)
esp_err_t data_logger_write_row(FILE *f, unsigned long long timestamp_ms,
                                float eeg, float ecg, float gsr, float temp_c);

#ifdef __cplusplus
}
#endif
