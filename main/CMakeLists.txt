# REQUIRES must stay accurate: with `MINIMAL_BUILD ON` this list defines exactly
# which components are compiled into the image. Keep it minimal so Wi-Fi, lwIP,
# esp_netif and wpa_supplicant stay out of the firmware.
idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES board
                             led_indicator
                             ir_types
                             ir_capture
                             ir_playback
                             ir_store
                             ble_link
                             protocol
                             bt
                             esp_driver_rmt
                             esp_driver_gpio
                             esp_driver_ledc
                             nvs_flash
                             spiffs
                             esp_timer)
