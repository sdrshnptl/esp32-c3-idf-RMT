idf_component_register(SRCS "src/ble_link.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES bt esp_hw_support esp_common log)
