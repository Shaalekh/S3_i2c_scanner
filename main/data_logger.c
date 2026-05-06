#include "data_logger.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "esp_spiffs.h"
#include "esp_log.h"

static const char *TAG = "data_logger";
static const char *MOUNT_PATH = "/spiffs";

esp_err_t data_logger_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MOUNT_PATH,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted. Total: %d, used: %d", (int)total, (int)used);
    }
    return ESP_OK;
}

esp_err_t data_logger_deinit(void)
{
    esp_err_t ret = esp_vfs_spiffs_unregister(NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS unmount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

FILE *data_logger_create_unique_file(char *out_name, size_t len)
{
    // Try data000.csv..data999.csv
    for (int i = 0; i < 1000; i++) {
        snprintf(out_name, len, "%s/data%03d.csv", MOUNT_PATH, i);
        struct stat st;
        if (stat(out_name, &st) != 0) {
            // file does not exist -> create
            FILE *f = fopen(out_name, "w");
            if (f != NULL) {
                // write header
                fprintf(f, "timestamp_ms,eeg,ecg,gsr,temp_c\n");
                fflush(f);
                ESP_LOGI(TAG, "Created %s", out_name);
                return f;
            }
            ESP_LOGE(TAG, "Failed to create %s", out_name);
            return NULL;
        }
    }
    ESP_LOGE(TAG, "Failed to create unique filename (exhausted attempts)");
    return NULL;
}

esp_err_t data_logger_write_row(FILE *f, unsigned long long timestamp_ms,
                                float eeg, float ecg, float gsr, float temp_c)
{
    if (f == NULL) return ESP_ERR_INVALID_ARG;
    int rc = fprintf(f, "%llu,%.6f,%.6f,%.6f,%.3f\n",
                     timestamp_ms, (double)eeg, (double)ecg, (double)gsr, (double)temp_c);
    if (rc < 0) return ESP_FAIL;
    fflush(f);
    return ESP_OK;
}
