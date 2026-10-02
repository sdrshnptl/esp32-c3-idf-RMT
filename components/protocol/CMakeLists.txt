idf_component_register(SRCS "src/protocol.c"
                    INCLUDE_DIRS "include"
                    REQUIRES ble_link
                             ir_types
                             ir_store
                             ir_capture
                             ir_playback
                             board
                             led_indicator
                    PRIV_REQUIRES esp_common log esp_timer esp_app_format freertos)
