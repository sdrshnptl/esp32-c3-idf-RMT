#include "board.h"

#include <stddef.h>

#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "board";

/* Kconfig booleans are only defined when set, so avoid relying on IS_ENABLED. */
#ifdef CONFIG_BOARD_STATUS_LED_ACTIVE_LOW
#define BOARD_LED_ACTIVE_LOW true
#else
#define BOARD_LED_ACTIVE_LOW false
#endif

static const board_pins_t s_pins = {
    .ir_rx = (gpio_num_t)CONFIG_BOARD_IR_RX_GPIO,
    .ir_tx = (gpio_num_t)CONFIG_BOARD_IR_TX_GPIO,
    .hotkey = (gpio_num_t)CONFIG_BOARD_HOTKEY_GPIO,
    .status_led = (gpio_num_t)CONFIG_BOARD_STATUS_LED_GPIO,
    .status_led_active_low = BOARD_LED_ACTIVE_LOW,
};

/**
 * @brief Pins that are unsafe to reuse for application functions.
 */
static bool pin_is_reserved(gpio_num_t pin)
{
    switch (pin) {
    case GPIO_NUM_2:  /* strapping */
    case GPIO_NUM_8:  /* strapping, carries the onboard LED */
    case GPIO_NUM_9:  /* BOOT strapping */
    case GPIO_NUM_18: /* USB-JTAG */
    case GPIO_NUM_19: /* USB-JTAG */
        return true;
    default:
        return false;
    }
}

esp_err_t board_init(void)
{
    const gpio_num_t pins[] = { s_pins.ir_rx, s_pins.ir_tx, s_pins.hotkey, s_pins.status_led };
    const char *names[] = { "ir_rx", "ir_tx", "hotkey", "status_led" };
    const size_t count = sizeof(pins) / sizeof(pins[0]);

    for (size_t i = 0; i < count; i++) {
        ESP_RETURN_ON_FALSE(GPIO_IS_VALID_GPIO(pins[i]), ESP_ERR_INVALID_ARG, TAG,
                            "invalid %s GPIO %d", names[i], (int)pins[i]);
        for (size_t j = i + 1; j < count; j++) {
            ESP_RETURN_ON_FALSE(pins[i] != pins[j], ESP_ERR_INVALID_ARG, TAG,
                                "%s and %s share GPIO %d", names[i], names[j], (int)pins[i]);
        }
    }

    /* Functional pins must not sit on strapping/USB pins; the status LED is exempt. */
    for (size_t i = 0; i < count - 1; i++) {
        if (pin_is_reserved(pins[i])) {
            ESP_LOGW(TAG, "%s is on reserved GPIO %d (strapping/USB) — verify the wiring",
                     names[i], (int)pins[i]);
        }
    }

    ESP_LOGI(TAG, "pins: ir_rx=%d ir_tx=%d hotkey=%d led=%d (%s)",
             (int)s_pins.ir_rx, (int)s_pins.ir_tx, (int)s_pins.hotkey, (int)s_pins.status_led,
             s_pins.status_led_active_low ? "active-low" : "active-high");
    return ESP_OK;
}

const board_pins_t *board_pins(void)
{
    return &s_pins;
}
