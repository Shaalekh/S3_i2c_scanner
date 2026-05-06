idf_component_register(SRCS "data_logger.c" "ads1115.c" "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver spiffs)
