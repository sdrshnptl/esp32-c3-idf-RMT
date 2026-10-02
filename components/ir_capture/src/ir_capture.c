#include "ir_capture.h"

#include <stddef.h>
#include <string.h>

#include "board.h"
#include "driver/rmt_rx.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "ir_capture";

/* 1 MHz resolution: one RMT tick is exactly one microsecond. */
#define IR_CAPTURE_RESOLUTION_HZ 1000000

/* Must be even and at least SOC_RMT_MEM_WORDS_PER_CHANNEL (48 on the ESP32-C3).
 * One block keeps the RMT footprint minimal; the ping-pong half is then 24 symbols,
 * i.e. the ISR drains the hardware FIFO roughly every 24 received symbols. */
#define IR_CAPTURE_RX_MEM_SYMBOLS 48

typedef struct {
    rmt_channel_handle_t rx_chan;
    esp_timer_handle_t window_timer;
    TaskHandle_t task;
    ir_capture_cb_t cb;
    void *cb_ctx;

    /* Written by the ISR, read by the capture task. */
    volatile size_t isr_symbols;
    volatile bool isr_is_last;

    bool initialized;
    bool armed; /* a rmt_receive() is outstanding */
    bool learning;
    bool overflowed;
    bool have_previous;
    uint32_t window_ms;
    int64_t last_frame_us;
    uint64_t received_frames;

    /* Static storage: the frame is ~1 KB, so it must never live on a stack. */
    rmt_symbol_word_t rx_buf[IR_FRAME_MAX_SYMBOLS];
    ir_frame_t frame;
    ir_frame_t previous;

    StaticTask_t task_buf;
    StackType_t task_stack[CONFIG_IR_CAPTURE_TASK_STACK_SIZE];
} ir_capture_ctx_t;

static ir_capture_ctx_t s_ctx;

/* ---- helpers ------------------------------------------------------------- */

static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void emit(ir_capture_event_t event, const ir_frame_t *frame)
{
    if (s_ctx.cb != NULL) {
        s_ctx.cb(event, frame, s_ctx.cb_ctx);
    }
}

/**
 * @brief Log the idle level of the receiver pin.
 *
 * A demodulating IR receiver (TSOP/VS1838 class) idles HIGH and pulls LOW during a burst,
 * so a pin that reads a permanent 0 means the module is unpowered or miswired. This turns
 * "no frames captured" from an ambiguous symptom into a concrete diagnosis.
 */
static void log_idle_level(const char *stage)
{
    const board_pins_t *pins = board_pins();
    int level = gpio_get_level(pins->ir_rx);
    ESP_LOGI(TAG, "%s: IR RX (gpio %d) idle level = %d%s", stage, (int)pins->ir_rx, level,
             level ? "" : " - UNEXPECTED, check receiver power/wiring");
}

/**
 * @brief Abort any outstanding reception and return the channel to a known idle state.
 *
 * rmt_disable() terminates an in-flight reception and leaves the channel in the INIT state;
 * rmt_enable() brings it back to ENABLE so it can be armed again. This matters because
 * calling rmt_receive() while a reception is already running is rejected with
 * ESP_ERR_INVALID_STATE - which is exactly what happened when the loopback self-test armed
 * the receiver and the application then armed it a second time.
 */
static esp_err_t disarm(void)
{
    ESP_RETURN_ON_ERROR(rmt_disable(s_ctx.rx_chan), TAG, "disable RX channel failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_ctx.rx_chan), TAG, "re-enable RX channel failed");
    s_ctx.armed = false;
    return ESP_OK;
}

static esp_err_t rearm(void)
{
    if (s_ctx.armed) {
        return ESP_OK; /* a reception is already in flight */
    }

    const rmt_receive_config_t cfg = {
        .signal_range_min_ns = CONFIG_IR_CAPTURE_GLITCH_FILTER_NS,
        .signal_range_max_ns = (uint32_t)CONFIG_IR_CAPTURE_FRAME_GAP_MS * 1000000u,
        /* Report intermediate chunks instead of truncating silently, so an oversized
         * frame surfaces as IR_CAPTURE_EVENT_OVERFLOW rather than lost data. */
        .flags.en_partial_rx = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_receive(s_ctx.rx_chan, s_ctx.rx_buf, sizeof(s_ctx.rx_buf), &cfg), TAG,
                        "arm receiver failed");
    s_ctx.armed = true;
    return ESP_OK;
}

static esp_err_t restart_window(void)
{
    esp_err_t err = esp_timer_stop(s_ctx.window_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "window timer stop failed");
    }
    return esp_timer_start_once(s_ctx.window_timer, (uint64_t)s_ctx.window_ms * 1000u);
}

/**
 * @brief Decide whether `cur` repeats `prev`.
 *
 * Two styles are covered:
 *  - the remote repeats the full frame (matched by ir_capture_frames_match), or
 *  - the remote sends a much shorter repeat code (e.g. NEC's 9 ms + 2.25 ms frame),
 *    detected as a frame with at most a third of the previous frame's edges.
 * Both must arrive inside the de-duplication window.
 */
static bool is_repeat_of_previous(const ir_frame_t *prev, const ir_frame_t *cur, int64_t delta_ms)
{
    if (delta_ms > CONFIG_IR_CAPTURE_DEDUP_WINDOW_MS) {
        return false;
    }
    if (ir_capture_frames_match(prev, cur)) {
        return true;
    }
    return (size_t)cur->edge_count * 3u <= (size_t)prev->edge_count;
}

static void process_frame(size_t symbol_count)
{
    if (symbol_count == 0 || symbol_count > IR_FRAME_MAX_SYMBOLS) {
        return;
    }

    ir_frame_t *frame = &s_ctx.frame;
    frame->start_level = s_ctx.rx_buf[0].level0;

    size_t edges = 0;
    for (size_t i = 0; i < symbol_count; i++) {
        frame->durations[edges++] = (uint16_t)s_ctx.rx_buf[i].duration0;
        frame->durations[edges++] = (uint16_t)s_ctx.rx_buf[i].duration1;
    }
    /* Drop the trailing entries the driver appends to mark end of reception. */
    while (edges > 0 && frame->durations[edges - 1] == 0) {
        edges--;
    }
    if (edges == 0) {
        return;
    }
    frame->edge_count = (uint16_t)edges;

    bool repeat = s_ctx.have_previous &&
                  is_repeat_of_previous(&s_ctx.previous, frame, now_ms() - (s_ctx.last_frame_us / 1000));

    s_ctx.previous = *frame;
    s_ctx.have_previous = true;
    s_ctx.last_frame_us = esp_timer_get_time();
    s_ctx.received_frames++;

    ESP_LOGD(TAG, "captured %u edges, start level %u%s", frame->edge_count, frame->start_level,
             repeat ? " (repeat)" : "");
    emit(repeat ? IR_CAPTURE_EVENT_REPEAT : IR_CAPTURE_EVENT_FRAME, frame);
}

/* ---- ISR ----------------------------------------------------------------- */

static bool IRAM_ATTR ir_capture_rx_done(rmt_channel_handle_t chan,
                                        const rmt_rx_done_event_data_t *edata, void *ctx)
{
    (void)chan;
    ir_capture_ctx_t *self = (ir_capture_ctx_t *)ctx;

    /* Only record the outcome here: converting symbols to durations is far too much
     * work for an ISR. The buffer stays valid until the next rmt_receive(). */
    self->isr_symbols = edata->num_symbols;
    self->isr_is_last = edata->flags.is_last;

    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(self->task, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

/* ---- task ---------------------------------------------------------------- */

static void ir_capture_task(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        size_t symbols = s_ctx.isr_symbols;
        bool is_last = s_ctx.isr_is_last;
        s_ctx.isr_symbols = 0;
        s_ctx.isr_is_last = false;

        /* is_last means the driver finished this reception and disabled the engine, so
         * nothing is outstanding any more. */
        if (is_last) {
            s_ctx.armed = false;
        }

        if (!s_ctx.learning) {
            continue; /* window closed while a frame was in flight */
        }

        if (!is_last) {
            /* The user buffer wrapped: the frame is longer than IR_FRAME_MAX_EDGES.
             * Do not re-arm - the driver keeps filling the same buffer until the
             * line goes idle, and that tail must be discarded. */
            s_ctx.overflowed = true;
            continue;
        }

        if (s_ctx.overflowed) {
            s_ctx.overflowed = false;
            ESP_LOGW(TAG, "frame dropped: longer than %d edges", IR_FRAME_MAX_EDGES);
            emit(IR_CAPTURE_EVENT_OVERFLOW, NULL);
        } else {
            process_frame(symbols);
        }

        /* Frame finished, receiver is idle again: extend the window and re-arm. */
        esp_err_t err = restart_window();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "window restart failed: %s", esp_err_to_name(err));
            s_ctx.learning = false;
            continue;
        }
        err = rearm();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "re-arm failed: %s", esp_err_to_name(err));
            s_ctx.learning = false;
        }
    }
}

static void ir_capture_window_expired(void *arg)
{
    (void)arg;
    if (!s_ctx.learning) {
        return;
    }
    s_ctx.learning = false;
    s_ctx.overflowed = false;
    ESP_LOGI(TAG, "learn window closed after %llu frame(s)", (unsigned long long)s_ctx.received_frames);
    log_idle_level("learn closed");
    if (disarm() != ESP_OK) {
        ESP_LOGW(TAG, "failed to disarm the receiver");
    }
    emit(IR_CAPTURE_EVENT_TIMEOUT, NULL);
}

/* ---- public API ---------------------------------------------------------- */

bool ir_capture_frames_match(const ir_frame_t *a, const ir_frame_t *b)
{
    if (a == NULL || b == NULL || a->edge_count == 0 || a->edge_count != b->edge_count) {
        return false;
    }
    if (a->start_level != b->start_level) {
        return false;
    }

    for (size_t i = 0; i < a->edge_count; i++) {
        uint32_t x = a->durations[i];
        uint32_t y = b->durations[i];
        uint32_t diff = (x > y) ? (x - y) : (y - x);
        uint32_t reference = (x < y) ? x : y;
        uint32_t limit = reference * (uint32_t)CONFIG_IR_CAPTURE_MATCH_TOLERANCE_PERCENT / 100u +
                         (uint32_t)CONFIG_IR_CAPTURE_MATCH_TOLERANCE_US;
        if (diff > limit) {
            return false;
        }
    }
    return true;
}

esp_err_t ir_capture_init(void)
{
    ESP_RETURN_ON_FALSE(!s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    const board_pins_t *pins = board_pins();

    const rmt_rx_channel_config_t rx_cfg = {
        .gpio_num = pins->ir_rx,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_CAPTURE_RESOLUTION_HZ,
        .mem_block_symbols = IR_CAPTURE_RX_MEM_SYMBOLS,
        .intr_priority = 0,
        /* Polarity is preserved in ir_frame_t::start_level instead of being inverted here. */
        .flags.invert_in = 0,
    };
    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&rx_cfg, &s_ctx.rx_chan), TAG,
                        "create RMT RX channel failed");

    const rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = ir_capture_rx_done,
    };
    ESP_RETURN_ON_ERROR(rmt_rx_register_event_callbacks(s_ctx.rx_chan, &cbs, &s_ctx), TAG,
                        "register RX callback failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_ctx.rx_chan), TAG, "enable RX channel failed");

    const esp_timer_create_args_t timer_args = {
        .callback = ir_capture_window_expired,
        .name = "ir_learn",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_ctx.window_timer), TAG,
                        "create window timer failed");

    s_ctx.task = xTaskCreateStatic(ir_capture_task, "ir_capture",
                                   CONFIG_IR_CAPTURE_TASK_STACK_SIZE, NULL,
                                   CONFIG_IR_CAPTURE_TASK_PRIORITY, s_ctx.task_stack, &s_ctx.task_buf);
    ESP_RETURN_ON_FALSE(s_ctx.task != NULL, ESP_ERR_NO_MEM, TAG, "create capture task failed");

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "ready (gpio %d, 1 MHz, filter %d ns, frame gap %d ms, max %d edges)",
             (int)pins->ir_rx, CONFIG_IR_CAPTURE_GLITCH_FILTER_NS, CONFIG_IR_CAPTURE_FRAME_GAP_MS,
             IR_FRAME_MAX_EDGES);
    log_idle_level("init");
    return ESP_OK;
}

esp_err_t ir_capture_set_callback(ir_capture_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    s_ctx.cb = cb;
    s_ctx.cb_ctx = ctx;
    return ESP_OK;
}

esp_err_t ir_capture_learn_start(uint32_t window_ms)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(window_ms > 0, ESP_ERR_INVALID_ARG, TAG, "window must be greater than zero");

    ESP_RETURN_ON_ERROR(ir_capture_learn_cancel(), TAG, "cancel failed");

    s_ctx.window_ms = window_ms;
    s_ctx.overflowed = false;
    s_ctx.have_previous = false;
    s_ctx.received_frames = 0;
    s_ctx.learning = true;

    ESP_RETURN_ON_ERROR(restart_window(), TAG, "window start failed");
    ESP_RETURN_ON_ERROR(rearm(), TAG, "arm failed");
    log_idle_level("learn armed");
    return ESP_OK;
}

esp_err_t ir_capture_learn_cancel(void)
{
    s_ctx.learning = false;
    s_ctx.overflowed = false;

    esp_err_t err = esp_timer_stop(s_ctx.window_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "window timer stop failed");
    }
    return disarm();
}

bool ir_capture_is_learning(void)
{
    return s_ctx.learning;
}

esp_err_t ir_capture_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    ESP_RETURN_ON_ERROR(ir_capture_learn_cancel(), TAG, "cancel failed");
    ESP_RETURN_ON_ERROR(rmt_disable(s_ctx.rx_chan), TAG, "disable RX channel failed");
    ESP_RETURN_ON_ERROR(rmt_del_channel(s_ctx.rx_chan), TAG, "delete RX channel failed");
    ESP_RETURN_ON_ERROR(esp_timer_delete(s_ctx.window_timer), TAG, "delete window timer failed");

    vTaskDelete(s_ctx.task);
    s_ctx.task = NULL;
    s_ctx.rx_chan = NULL;
    s_ctx.window_timer = NULL;
    s_ctx.initialized = false;
    return ESP_OK;
}
