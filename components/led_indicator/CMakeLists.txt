idf_component_register(SRCS "src/led_indicator.c"
                    INCLUDE_DIRS "include"
                    REQUIRES board esp_driver_ledc
                    PRIV_REQUIRES esp_timer esp_common log)
