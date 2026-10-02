idf_component_register(SRCS "src/ir_store.c"
                    INCLUDE_DIRS "include"
                    REQUIRES ir_types nvs_flash spiffs
                    PRIV_REQUIRES esp_common log)
