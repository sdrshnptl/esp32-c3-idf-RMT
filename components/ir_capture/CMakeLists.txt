idf_component_register(SRCS "src/ir_capture.c"
                    INCLUDE_DIRS "include"
                    REQUIRES ir_types board esp_driver_rmt
                    PRIV_REQUIRES esp_timer esp_common log)
