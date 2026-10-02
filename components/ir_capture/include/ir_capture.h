#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Events reported by the IR capture component.
 */
typedef enum {
    IR_CAPTURE_EVENT_FRAME = 0, /*!< A frame distinct from the previous one was captured */
    IR_CAPTURE_EVENT_REPEAT,    /*!< The frame is a repeat of the previously captured frame */
    IR_CAPTURE_EVENT_TIMEOUT,   /*!< The learn window expired without a new frame */
    IR_CAPTURE_EVENT_OVERFLOW,  /*!< The frame was longer than the edge budget and was dropped */
    IR_CAPTURE_EVENT_MAX,
} ir_capture_event_t;

/**
 * @brief Capture callback.
 *
 * @param event  What happened.
 * @param frame  Captured frame, valid only for FRAME and REPEAT events. The pointer is
 *               owned by the component and is reused on the next capture - copy it if you
 *               need to keep it.
 * @param ctx    Context registered with ir_capture_set_callback().
 *
 * @note Invoked from the capture task, or from the esp_timer task for TIMEOUT. It is
 *       therefore safe to call blocking APIs (NVS, BLE, LED) from it, but it must not block
 *       for long: the receiver is only re-armed after the callback returns.
 */
typedef void (*ir_capture_cb_t)(ir_capture_event_t event, const ir_frame_t *frame, void *ctx);

/**
 * @brief Create the RMT RX channel and start the capture task.
 *
 * Requires board_init() to have run.
 */
esp_err_t ir_capture_init(void);

/**
 * @brief Register (or clear, with NULL) the capture callback.
 */
esp_err_t ir_capture_set_callback(ir_capture_cb_t cb, void *ctx);

/**
 * @brief Arm the receiver and start (or extend) the learn window.
 *
 * @param window_ms Inactivity window. Every captured frame restarts it, so a user can learn
 *                  several buttons in one session. After it expires, IR_CAPTURE_EVENT_TIMEOUT
 *                  is reported and the receiver is disarmed.
 */
esp_err_t ir_capture_learn_start(uint32_t window_ms);

/**
 * @brief Disarm the receiver and stop the learn window. No event is reported.
 */
esp_err_t ir_capture_learn_cancel(void);

/** @brief True while the receiver is armed. */
bool ir_capture_is_learning(void);

/**
 * @brief Compare two frames within the configured tolerances.
 *
 * Exposed because the dashboard also needs to tell two learned buttons apart.
 */
bool ir_capture_frames_match(const ir_frame_t *a, const ir_frame_t *b);

/** @brief Disarm, release the RMT channel and stop the capture task. */
esp_err_t ir_capture_deinit(void);

#ifdef __cplusplus
}
#endif
