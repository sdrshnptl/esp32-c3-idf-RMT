#include "led_indicator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "led";

#define LED_SPEED_MODE LEDC_LOW_SPEED_MODE
#define LED_TIMER      LEDC_TIMER_0
#define LED_CHANNEL    LEDC_CHANNEL_0

#define LED_DUTY_BITS ((ledc_timer_bit_t)CONFIG_LED_INDICATOR_PWM_RESOLUTION_BITS)
#define LED_DUTY_FULL ((uint32_t)((1u << CONFIG_LED_INDICATOR_PWM_RESOLUTION_BITS) - 1u))
#define LED_TICK_MS   ((uint32_t)CONFIG_LED_INDICATOR_TICK_MS)
#define LED_ERROR_MS  5000u

#define LED_BREATH_HALF_STEPS 20
#define LED_BREATH_STEP_MS    50

typedef struct {
    uint32_t duty;    /*!< 0 = off, LED_DUTY_FULL = full brightness */
    uint16_t dur_ms;  /*!< 0 = hold this step indefinitely */
} led_step_t;

typedef struct {
    const led_step_t *steps;
    size_t count;
    bool loop;
} led_pattern_t;

/* ---- Pattern tables ------------------------------------------------------ */

static const led_step_t s_off_steps[] = { { .duty = 0, .dur_ms = 0 } };

static const led_step_t s_connected_steps[] = { { .duty = LED_DUTY_FULL / 4u, .dur_ms = 0 } };

static const led_step_t s_learning_steps[] = {
    { .duty = LED_DUTY_FULL, .dur_ms = 100 },
    { .duty = 0, .dur_ms = 100 },
};

static const led_step_t s_error_steps[] = {
    { .duty = LED_DUTY_FULL, .dur_ms = 62 },
    { .duty = 0, .dur_ms = 62 },
};

static const led_step_t s_reset_steps[] = {
    { .duty = LED_DUTY_FULL, .dur_ms = 50 },
    { .duty = 0, .dur_ms = 50 },
};

static const led_step_t s_boot_steps[] = {
    { .duty = LED_DUTY_FULL, .dur_ms = 120 }, { .duty = 0, .dur_ms = 120 },
    { .duty = LED_DUTY_FULL, .dur_ms = 120 }, { .duty = 0, .dur_ms = 120 },
    { .duty = LED_DUTY_FULL, .dur_ms = 120 }, { .duty = 0, .dur_ms = 120 },
};

static const led_step_t s_capture_ok_steps[] = {
    { .duty = LED_DUTY_FULL, .dur_ms = 250 },
    { .duty = 0, .dur_ms = 0 },
};

static const led_step_t s_ir_frame_steps[] = {
    { .duty = LED_DUTY_FULL, .dur_ms = 30 },
    { .duty = 0, .dur_ms = 0 },
};

static const led_step_t s_hotkey_step_steps[] = {
    { .duty = LED_DUTY_FULL, .dur_ms = 80 },
    { .duty = 0, .dur_ms = 80 },
    { .duty = LED_DUTY_FULL, .dur_ms = 80 },
    { .duty = 0, .dur_ms = 0 },
};

/* Filled in at init: the ramp depends on the configured PWM resolution. */
static led_step_t s_breath_steps[LED_BREATH_HALF_STEPS * 2];

/* Non-const because LED_STATE_ADVERTISING is populated at init. */
static led_pattern_t s_patterns[LED_STATE_MAX] = {
    [LED_STATE_OFF] = { .steps = s_off_steps, .count = 1, .loop = true },
    [LED_STATE_BOOT] = { .steps = s_boot_steps, .count = 6, .loop = false },
    [LED_STATE_CONNECTED] = { .steps = s_connected_steps, .count = 1, .loop = true },
    [LED_STATE_LEARNING] = { .steps = s_learning_steps, .count = 2, .loop = true },
    [LED_STATE_ERROR] = { .steps = s_error_steps, .count = 2, .loop = true },
    [LED_STATE_FACTORY_RESET] = { .steps = s_reset_steps, .count = 2, .loop = true },
};

static const led_pattern_t s_pulses[LED_PULSE_MAX] = {
    [LED_PULSE_CAPTURE_OK] = { .steps = s_capture_ok_steps, .count = 2, .loop = false },
    [LED_PULSE_IR_FRAME] = { .steps = s_ir_frame_steps, .count = 2, .loop = false },
    [LED_PULSE_HOTKEY_STEP] = { .steps = s_hotkey_step_steps, .count = 4, .loop = false },
};

/* ---- Engine state (all static, no runtime allocation in the hot path) ----- */

static esp_timer_handle_t s_tick_timer;
static const led_pattern_t *s_active;
static led_state_t s_base_state = LED_STATE_OFF;
static size_t s_step;
static uint32_t s_elapsed_ms;
static uint32_t s_deadline_ms;
static bool s_initialized;
static bool s_duty_error_logged;

static void led_apply(uint32_t duty)
{
    if (duty > LED_DUTY_FULL) {
        duty = LED_DUTY_FULL;
    }

    /* Note: ledc_set_duty_and_update() goes through the LEDC fade path and fails
     * unless the fade service is installed. The plain duty path is used here.
     * Only the pattern engine touches this channel, so the non-thread-safe
     * ledc_set_duty()/ledc_update_duty() pair is safe. */
    esp_err_t err = ledc_set_duty(LED_SPEED_MODE, LED_CHANNEL, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(LED_SPEED_MODE, LED_CHANNEL);
    }
    if (err != ESP_OK && !s_duty_error_logged) {
        s_duty_error_logged = true;
        ESP_LOGE(TAG, "LEDC duty update failed: %s", esp_err_to_name(err));
    }
}

static void led_start_pattern(const led_pattern_t *pattern)
{
    s_active = pattern;
    s_step = 0;
    s_elapsed_ms = 0;
    led_apply(pattern->steps[0].duty);
}

static void led_tick(void *arg)
{
    (void)arg;
    if (s_active == NULL) {
        return;
    }

    if (s_deadline_ms > 0) {
        s_deadline_ms = (s_deadline_ms > LED_TICK_MS) ? (s_deadline_ms - LED_TICK_MS) : 0;
        if (s_deadline_ms == 0) {
            led_start_pattern(&s_patterns[s_base_state]);
            return;
        }
    }

    if (s_active->steps[s_step].dur_ms == 0) {
        return; /* hold the current duty */
    }

    s_elapsed_ms += LED_TICK_MS;
    if (s_elapsed_ms < s_active->steps[s_step].dur_ms) {
        return;
    }

    s_elapsed_ms = 0;
    s_step++;
    if (s_step >= s_active->count) {
        if (!s_active->loop) {
            led_start_pattern(&s_patterns[s_base_state]);
            return;
        }
        s_step = 0;
    }
    led_apply(s_active->steps[s_step].duty);
}

static void led_build_breath_pattern(void)
{
    const uint32_t peak = LED_DUTY_FULL / 8u; /* ~12.5 % brightness */
    const size_t total = LED_BREATH_HALF_STEPS * 2;

    for (size_t i = 0; i < LED_BREATH_HALF_STEPS; i++) {
        uint32_t duty = (peak * (uint32_t)(i + 1)) / LED_BREATH_HALF_STEPS;
        s_breath_steps[i].duty = duty;
        s_breath_steps[i].dur_ms = LED_BREATH_STEP_MS;
        s_breath_steps[total - 1 - i].duty = duty;
        s_breath_steps[total - 1 - i].dur_ms = LED_BREATH_STEP_MS;
    }

    s_patterns[LED_STATE_ADVERTISING].steps = s_breath_steps;
    s_patterns[LED_STATE_ADVERTISING].count = total;
    s_patterns[LED_STATE_ADVERTISING].loop = true;
}

esp_err_t led_indicator_init(void)
{
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    const board_pins_t *pins = board_pins();

    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LED_SPEED_MODE,
        .duty_resolution = LED_DUTY_BITS,
        .timer_num = LED_TIMER,
        .freq_hz = CONFIG_LED_INDICATOR_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "LEDC timer config failed");

    const ledc_channel_config_t channel_cfg = {
        .gpio_num = (int)pins->status_led,
        .speed_mode = LED_SPEED_MODE,
        .channel = LED_CHANNEL,
        .timer_sel = LED_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE,
        .flags = { .output_invert = pins->status_led_active_low ? 1u : 0u },
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_cfg), TAG, "LEDC channel config failed");

    led_build_breath_pattern();

    const esp_timer_create_args_t timer_args = {
        .callback = led_tick,
        .name = "led_tick",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_tick_timer), TAG, "timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_tick_timer, (uint64_t)LED_TICK_MS * 1000u), TAG,
                        "timer start failed");

    s_base_state = LED_STATE_OFF;
    s_initialized = true;
    led_start_pattern(&s_patterns[LED_STATE_OFF]);

    ESP_LOGI(TAG, "LED indication ready (gpio %d, %s, %d Hz, %u-bit)",
             (int)pins->status_led, pins->status_led_active_low ? "active-low" : "active-high",
             CONFIG_LED_INDICATOR_PWM_FREQ_HZ, CONFIG_LED_INDICATOR_PWM_RESOLUTION_BITS);
    return ESP_OK;
}

esp_err_t led_indicator_set_state(led_state_t state)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(state >= LED_STATE_OFF && state < LED_STATE_MAX, ESP_ERR_INVALID_ARG, TAG,
                        "invalid state %d", (int)state);

    if (state != LED_STATE_ERROR) {
        s_base_state = state;
    }
    s_deadline_ms = (state == LED_STATE_ERROR) ? LED_ERROR_MS : 0;
    led_start_pattern(&s_patterns[state]);
    return ESP_OK;
}

esp_err_t led_indicator_pulse(led_pulse_t pulse)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(pulse >= 0 && pulse < LED_PULSE_MAX, ESP_ERR_INVALID_ARG, TAG,
                        "invalid pulse %d", (int)pulse);

    s_deadline_ms = 0;
    led_start_pattern(&s_pulses[pulse]);
    return ESP_OK;
}

esp_err_t led_indicator_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    ESP_RETURN_ON_ERROR(esp_timer_stop(s_tick_timer), TAG, "timer stop failed");
    ESP_RETURN_ON_ERROR(esp_timer_delete(s_tick_timer), TAG, "timer delete failed");
    ESP_RETURN_ON_ERROR(ledc_stop(LED_SPEED_MODE, LED_CHANNEL, 0), TAG, "LEDC stop failed");

    s_tick_timer = NULL;
    s_active = NULL;
    s_initialized = false;
    return ESP_OK;
}
