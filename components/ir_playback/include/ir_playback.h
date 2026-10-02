#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "ir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the RMT TX channel and the copy encoder.
 *
 * Requires board_init() to have run.
 */
esp_err_t ir_playback_init(void);

/**
 * @brief Transmit a captured frame, optionally repeating it.
 *
 * @param frame  Frame to send. Its `start_level` is used to work out the required
 *               polarity: a demodulating receiver reports a burst as a low level, while the
 *               IR emitter is driven active-high, so the levels are flipped when needed.
 * @param params Carrier and repeat settings. NULL selects IR_TX_PARAMS_DEFAULT().
 *
 * @note **Blocking.** Returns once every repeat has been transmitted, so a frame with
 *       repeats can hold the caller for a second or more. Call it from a dedicated worker
 *       task or an event handler that is allowed to block - never from an ISR or a
 *       time-critical callback.
 */
esp_err_t ir_playback_send(const ir_frame_t *frame, const ir_tx_params_t *params);

/**
 * @brief Abort the in-flight transmission, if any.
 *
 * @note Not synchronised with a concurrent ir_playback_send(); use it from the same task
 *       that performs the transmission.
 */
esp_err_t ir_playback_stop(void);

/** @brief Whether a transmission is currently in progress. */
bool ir_playback_is_busy(void);

/** @brief Dispose of the encoder and the RMT channel. */
esp_err_t ir_playback_deinit(void);

#ifdef __cplusplus
}
#endif
