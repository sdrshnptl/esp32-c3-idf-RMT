#include "ir_playback.h"

#include <stddef.h>
#include <string.h>

#include "board.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "ir_playback";

/* 1 MHz resolution: one RMT tick is exactly one microsecond. */
#define IR_PLAYBACK_RESOLUTION_HZ 1000000

/* The RMT duration field is 15 bits, so one level cannot exceed 32767 us at 1 MHz.
 * Capture can never produce more than this (the end-of-frame gap is capped at 32 ms),
 * but imported data might, so longer levels are split into consecutive pieces. */
#define IR_PLAYBACK_MAX_DURATION_US 32767u

typedef struct {
    rmt_channel_handle_t tx_chan;
    rmt_encoder_handle_t copy_encoder;
    bool initialized;
    bool carrier_applied;
    uint32_t carrier_hz;
    uint8_t duty_percent;

    /* Payload must stay valid until rmt_tx_wait_all_done() returns, hence static. */
    rmt_symbol_word_t symbols[IR_FRAME_MAX_EDGES];
    size_t symbol_count;
} ir_playback_ctx_t;

static ir_playback_ctx_t s_ctx;

/**
 * @brief Convert a frame into RMT symbols, splitting over-long levels.
 *
 * @param invert  Flip every level. Set when the frame came from a demodulating receiver
 *                (burst = low) but the emitter is driven active-high.
 */
static esp_err_t build_symbols(const ir_frame_t *frame, bool invert)
{
    memset(s_ctx.symbols, 0, sizeof(s_ctx.symbols));
    s_ctx.symbol_count = 0;

    size_t pieces = 0;
    for (size_t i = 0; i < frame->edge_count; i++) {
        uint32_t level = (i & 1u) ? !frame->start_level : frame->start_level;
        if (invert) {
            level ^= 1u;
        }

        uint32_t remaining = frame->durations[i];
        while (remaining > 0) {
            uint32_t piece = (remaining > IR_PLAYBACK_MAX_DURATION_US) ? IR_PLAYBACK_MAX_DURATION_US
                                                                      : remaining;
            remaining -= piece;

            size_t index = pieces / 2u;
            ESP_RETURN_ON_FALSE(index < IR_FRAME_MAX_EDGES, ESP_ERR_INVALID_SIZE, TAG,
                                "frame needs more than %d symbols", IR_FRAME_MAX_EDGES);

            rmt_symbol_word_t *symbol = &s_ctx.symbols[index];
            if ((pieces & 1u) == 0) {
                symbol->level0 = level;
                symbol->duration0 = piece;
            } else {
                symbol->level1 = level;
                symbol->duration1 = piece;
            }
            pieces++;
        }
    }

    /* Round up so a trailing half-filled symbol is still transmitted. */
    s_ctx.symbol_count = (pieces + 1u) / 2u;
    ESP_RETURN_ON_FALSE(s_ctx.symbol_count > 0, ESP_ERR_INVALID_ARG, TAG, "frame has no durations");
    return ESP_OK;
}

static esp_err_t apply_carrier_if_needed(const ir_tx_params_t *params)
{
    if (s_ctx.carrier_applied && s_ctx.carrier_hz == params->carrier_hz &&
        s_ctx.duty_percent == params->duty_percent) {
        return ESP_OK;
    }

    if (params->carrier_hz == 0) {
        /* A NULL config disables modulation entirely. */
        ESP_RETURN_ON_ERROR(rmt_apply_carrier(s_ctx.tx_chan, NULL), TAG, "disable carrier failed");
    } else {
        const rmt_carrier_config_t carrier = {
            .frequency_hz = params->carrier_hz,
            /* duty_cycle is a 0..1 fraction, despite the header comment claiming 0~100%. */
            .duty_cycle = (float)params->duty_percent / 100.0f,
        };
        ESP_RETURN_ON_ERROR(rmt_apply_carrier(s_ctx.tx_chan, &carrier), TAG, "apply carrier failed");
    }

    s_ctx.carrier_hz = params->carrier_hz;
    s_ctx.duty_percent = params->duty_percent;
    s_ctx.carrier_applied = true;
    return ESP_OK;
}

esp_err_t ir_playback_init(void)
{
    ESP_RETURN_ON_FALSE(!s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    const board_pins_t *pins = board_pins();

    const rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = pins->ir_tx,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_PLAYBACK_RESOLUTION_HZ,
        .mem_block_symbols = CONFIG_IR_PLAYBACK_TX_MEM_SYMBOLS,
        .trans_queue_depth = 4,
        .intr_priority = 0,
        /* Polarity is handled per frame in build_symbols(), not globally here. */
        .flags.invert_out = 0,
        .flags.init_level = 0,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &s_ctx.tx_chan), TAG,
                        "create RMT TX channel failed");

    const rmt_copy_encoder_config_t encoder_cfg = {};
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&encoder_cfg, &s_ctx.copy_encoder), TAG,
                        "create copy encoder failed");

    ESP_RETURN_ON_ERROR(rmt_enable(s_ctx.tx_chan), TAG, "enable TX channel failed");

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "ready (gpio %d, 1 MHz, %d symbol blocks)",
             (int)pins->ir_tx, CONFIG_IR_PLAYBACK_TX_MEM_SYMBOLS);
    return ESP_OK;
}

esp_err_t ir_playback_send(const ir_frame_t *frame, const ir_tx_params_t *params)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(frame != NULL && frame->edge_count > 0, ESP_ERR_INVALID_ARG, TAG,
                        "invalid frame");

    const ir_tx_params_t defaults = IR_TX_PARAMS_DEFAULT();
    if (params == NULL) {
        params = &defaults;
    }

    /* A demodulating receiver reports a burst as a LOW level; the emitter is driven
     * active-high, so the stored levels must be flipped in that case. */
    bool invert = (frame->start_level == 0);

    ESP_RETURN_ON_ERROR(build_symbols(frame, invert), TAG, "symbol build failed");
    ESP_RETURN_ON_ERROR(apply_carrier_if_needed(params), TAG, "carrier setup failed");

    const rmt_transmit_config_t tx_cfg = {
        .loop_count = 0, /* send once; repeats are driven from here */
        .flags.eot_level = 0,
    };

    for (uint32_t i = 0; i <= params->repeats; i++) {
        ESP_RETURN_ON_ERROR(rmt_transmit(s_ctx.tx_chan, s_ctx.copy_encoder, s_ctx.symbols,
                                        s_ctx.symbol_count * sizeof(rmt_symbol_word_t), &tx_cfg),
                            TAG, "transmit failed");
        ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(s_ctx.tx_chan, CONFIG_IR_PLAYBACK_TX_TIMEOUT_MS),
                            TAG, "transmit did not complete");

        if (i < params->repeats && params->repeat_gap_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(params->repeat_gap_ms));
        }
    }

    ESP_LOGD(TAG, "sent %u edges (%u symbols) x%u at %" PRIu32 " Hz", frame->edge_count,
             (unsigned)s_ctx.symbol_count, (unsigned)(params->repeats + 1), params->carrier_hz);
    return ESP_OK;
}

esp_err_t ir_playback_stop(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    /* Disabling the channel terminates an in-flight transmission; re-enabling makes it
     * usable again. rmt_tx_wait_all_done() in the sender then reports a failure. */
    ESP_RETURN_ON_ERROR(rmt_disable(s_ctx.tx_chan), TAG, "disable TX channel failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_ctx.tx_chan), TAG, "re-enable TX channel failed");

    /* The carrier registers are cleared by the channel restart. */
    s_ctx.carrier_applied = false;
    return ESP_OK;
}

bool ir_playback_is_busy(void)
{
    return false;
}

esp_err_t ir_playback_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_ctx.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    ESP_RETURN_ON_ERROR(rmt_disable(s_ctx.tx_chan), TAG, "disable TX channel failed");
    ESP_RETURN_ON_ERROR(rmt_del_encoder(s_ctx.copy_encoder), TAG, "delete encoder failed");
    ESP_RETURN_ON_ERROR(rmt_del_channel(s_ctx.tx_chan), TAG, "delete TX channel failed");

    s_ctx.copy_encoder = NULL;
    s_ctx.tx_chan = NULL;
    s_ctx.initialized = false;
    return ESP_OK;
}
