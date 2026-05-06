idf_component_register(SRCS "data_logger.c" "ads1115.c" "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver spiffs esp_wifi esp_http_server esp_netif nvs_flash esp_timer u8g2)
