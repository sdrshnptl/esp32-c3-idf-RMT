#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of level durations (marks + spaces) in one captured frame. */
#define IR_FRAME_MAX_EDGES CONFIG_IR_FRAME_MAX_EDGES

/** Maximum number of RMT symbols a full frame can occupy. */
#define IR_FRAME_MAX_SYMBOLS (IR_FRAME_MAX_EDGES / 2)

/**
 * @brief A captured infrared frame stored as a sequence of level durations.
 *
 * `durations[0]` has the level `start_level` and every following entry alternates
 * level. Durations are in microseconds. Storing levels explicitly (instead of
 * assuming mark/space ordering) makes the frame immune to receiver polarity.
 */
typedef struct {
    uint8_t start_level;                    /*!< Level of durations[0] */
    uint16_t edge_count;                    /*!< Valid entries in durations[] */
    uint16_t durations[IR_FRAME_MAX_EDGES]; /*!< Level durations, microseconds */
} ir_frame_t;

/**
 * @brief Playback parameters for a stored frame.
 */
typedef struct {
    uint32_t carrier_hz;    /*!< TX carrier frequency; 0 disables modulation */
    uint8_t duty_percent;   /*!< TX carrier duty cycle, percent (0-100) */
    uint8_t repeats;        /*!< Additional repeats of the frame (0 = send once) */
    uint16_t repeat_gap_ms; /*!< Gap between repeats, milliseconds */
} ir_tx_params_t;

/** Sensible defaults for a 38 kHz modulated IR emitter. */
#define IR_TX_PARAMS_DEFAULT() \
    { .carrier_hz = 38000, .duty_percent = 33, .repeats = 0, .repeat_gap_ms = 40 }

#ifdef __cplusplus
}
#endif
