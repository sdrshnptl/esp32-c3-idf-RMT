#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Steady indication states.
 *
 * The onboard LED is a single blue emitter, so states are distinguished by
 * brightness and blink pattern rather than colour.
 */
typedef enum {
    LED_STATE_OFF = 0,       /*!< Dark */
    LED_STATE_BOOT,          /*!< 3 quick flashes, then returns to the base state */
    LED_STATE_ADVERTISING,   /*!< Slow breathing: waiting for a BLE central */
    LED_STATE_CONNECTED,     /*!< Solid dim: BLE link established */
    LED_STATE_LEARNING,      /*!< 5 Hz blink: waiting for an IR frame */
    LED_STATE_ERROR,         /*!< 8 Hz blink for a few seconds, then reverts */
    LED_STATE_FACTORY_RESET, /*!< Fast strobe: factory reset in progress */
    LED_STATE_MAX,
} led_state_t;

/**
 * @brief One-shot indications that temporarily override the current state.
 */
typedef enum {
    LED_PULSE_CAPTURE_OK = 0, /*!< Single 250 ms flash: frame captured */
    LED_PULSE_IR_FRAME,       /*!< Single 30 ms flash: frame transmitted */
    LED_PULSE_HOTKEY_STEP,    /*!< Double 80 ms pulse: hotkey step executed */
    LED_PULSE_MAX,
} led_pulse_t;

/**
 * @brief Initialise the LEDC PWM backend and start the pattern engine.
 *
 * The LED pin and polarity come from the `board` component.
 */
esp_err_t led_indicator_init(void);

/**
 * @brief Switch to a steady state. Pulses in flight are cancelled.
 */
esp_err_t led_indicator_set_state(led_state_t state);

/**
 * @brief Play a one-shot indication, then return to the current steady state.
 */
esp_err_t led_indicator_pulse(led_pulse_t pulse);

/**
 * @brief Stop the pattern engine and release the LEDC channel.
 */
esp_err_t led_indicator_deinit(void);

#ifdef __cplusplus
}
#endif
