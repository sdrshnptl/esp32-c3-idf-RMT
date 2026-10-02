#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Physical pin assignment of the board.
 *
 * Only this component is allowed to translate logical functions into GPIO
 * numbers. Everything else must ask for pins through this API.
 */
typedef struct {
    gpio_num_t ir_rx;             /*!< Demodulating IR receiver output */
    gpio_num_t ir_tx;             /*!< IR emitter driver (transistor/MOSFET gate) */
    gpio_num_t hotkey;            /*!< Hotkey push button, active-low to GND */
    gpio_num_t status_led;        /*!< Onboard status LED */
    bool status_led_active_low;   /*!< True when a low level turns the LED on */
} board_pins_t;

/**
 * @brief Validate the configured pin map and log it.
 *
 * Rejects invalid or duplicated pins. Warns when a functional pin lands on a
 * strapping or USB-JTAG pin.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on an invalid pin map.
 */
esp_err_t board_init(void);

/**
 * @brief Get the board pin map (valid after board_init()).
 */
const board_pins_t *board_pins(void);

#ifdef __cplusplus
}
#endif
