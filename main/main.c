/*
 * ESP32-C3 IR Remote Controller — application entry point.
 *
 * This file stays thin on purpose: it wires components together in the correct
 * order and then returns. All logic lives in components/.
 */

#include <inttypes.h>
#include <string.h>

#include "board.h"
#include "ble_link.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ir_capture.h"
#include "ir_playback.h"
#include "ir_store.h"
#include "led_indicator.h"
#include "protocol.h"
#include "sdkconfig.h"

static const char *TAG = "ir-remote";

#if CONFIG_APP_BRINGUP_IR_LOOPBACK
/* Loopback self-test state; written by the capture callback, read by app_main. */
static volatile bool s_loopback_seen;
static ir_frame_t s_loopback_captured;

/* A distinctive NEC-shaped burst: 9 ms / 4.5 ms leader, then alternating 560/1690 us. */
static const uint16_t s_loopback_pattern[] = {
    9000, 4500, 560, 560, 560, 1690, 560, 560, 560, 1690,
    560,  560,  560, 1690, 560, 560, 560, 1690,
};
#endif

#if CONFIG_APP_BRINGUP_IR_LEARN || CONFIG_APP_BRINGUP_IR_LOOPBACK
/**
 * @brief Bring-up aid for the ir_capture milestone.
 *
 * Logs what the receiver sees so the RMT driver can be validated before the BLE
 * dashboard exists. Replaced by the BLE-driven learn flow in the transport
 * milestone.
 */
static void bringup_report(ir_capture_event_t event, const ir_frame_t *frame, void *ctx)
{
    (void)ctx;

    switch (event) {
    case IR_CAPTURE_EVENT_FRAME: {
        uint32_t total_us = 0;
        for (size_t i = 0; i < frame->edge_count; i++) {
            total_us += frame->durations[i];
        }
        ESP_LOGI(TAG, "IR frame: %u edges, start level %u, total %" PRIu32 " us",
                 (unsigned)frame->edge_count, (unsigned)frame->start_level, total_us);
        if (frame->edge_count >= 6) {
            ESP_LOGI(TAG, "  head: %u %u %u %u %u %u", (unsigned)frame->durations[0],
                     (unsigned)frame->durations[1], (unsigned)frame->durations[2],
                     (unsigned)frame->durations[3], (unsigned)frame->durations[4],
                     (unsigned)frame->durations[5]);
        }
        if (led_indicator_pulse(LED_PULSE_CAPTURE_OK) != ESP_OK) {
            ESP_LOGW(TAG, "capture pulse failed");
        }
#if CONFIG_APP_BRINGUP_IR_LOOPBACK
        s_loopback_captured = *frame;
        s_loopback_seen = true;
#endif
        break;
    }
    case IR_CAPTURE_EVENT_REPEAT:
        ESP_LOGI(TAG, "IR repeat: %u edges (ignored)", (unsigned)frame->edge_count);
#if CONFIG_APP_BRINGUP_IR_LOOPBACK
        s_loopback_captured = *frame;
        s_loopback_seen = true;
#endif
        break;
    case IR_CAPTURE_EVENT_OVERFLOW:
        ESP_LOGW(TAG, "IR frame rejected: longer than the configured edge budget");
        if (led_indicator_set_state(LED_STATE_ERROR) != ESP_OK) {
            ESP_LOGW(TAG, "error indication failed");
        }
        break;
    case IR_CAPTURE_EVENT_TIMEOUT:
        ESP_LOGI(TAG, "IR learn window closed");
        if (led_indicator_set_state(LED_STATE_ADVERTISING) != ESP_OK) {
            ESP_LOGW(TAG, "LED state change failed");
        }
        break;
    default:
        break;
    }
}
#endif /* CONFIG_APP_BRINGUP_IR_LEARN || CONFIG_APP_BRINGUP_IR_LOOPBACK */

#if CONFIG_APP_BRINGUP_IR_LOOPBACK
/**
 * @brief One loopback attempt with a given burst polarity. Returns true on a full match.
 */
#define LOOPBACK_ATTEMPTS 8

/**
 * @brief Compare a transmitted frame with a received one, tolerating capture artefacts.
 *
 * The receiver consistently drops the final space: after the last mark the line stays idle,
 * so that space merges into the end-of-frame gap. A strict ir_frame_t equality check
 * therefore always fails. Compare the overlapping leading edges with a tolerant margin and
 * allow a small edge-count difference instead.
 *
 * This is a bring-up aid and deliberately looser than ir_capture_frames_match(), which stays
 * strict because it guards button de-duplication.
 */
static bool loopback_frames_close(const ir_frame_t *tx, const ir_frame_t *rx)
{
    if (rx->edge_count < 4 || tx->edge_count > rx->edge_count + 2) {
        return false;
    }

    size_t overlap = (tx->edge_count < rx->edge_count) ? tx->edge_count : rx->edge_count;
    for (size_t i = 0; i < overlap; i++) {
        uint32_t a = tx->durations[i];
        uint32_t b = rx->durations[i];
        uint32_t diff = (a > b) ? (a - b) : (b - a);
        uint32_t limit = ((a < b) ? a : b) * 20u / 100u + 150u;
        if (diff > limit) {
            return false;
        }
    }
    return true;
}

static bool loopback_attempt(uint8_t start_level)
{
    ir_frame_t frame = { 0 };
    frame.start_level = start_level;
    frame.edge_count = sizeof(s_loopback_pattern) / sizeof(s_loopback_pattern[0]);
    for (size_t i = 0; i < frame.edge_count; i++) {
        frame.durations[i] = s_loopback_pattern[i];
    }

    ir_tx_params_t params = IR_TX_PARAMS_DEFAULT();

    s_loopback_seen = false;
    if (ir_capture_learn_start(5000) != ESP_OK) {
        ESP_LOGW(TAG, "loopback: could not arm the receiver");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50)); /* let the receiver settle */

    if (ir_playback_send(&frame, &params) != ESP_OK) {
        ESP_LOGW(TAG, "loopback: transmit failed");
        return false;
    }

    /* The receiver only reports the frame once the line has been idle for the
     * configured end-of-frame gap, so allow some slack here. */
    for (int i = 0; i < 40 && !s_loopback_seen; i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    if (!s_loopback_seen) {
        return false;
    }
    if (loopback_frames_close(&frame, &s_loopback_captured)) {
        return true;
    }
    ESP_LOGW(TAG, "loopback: received %u edges, transmitted %u - timings differ",
             (unsigned)s_loopback_captured.edge_count, (unsigned)frame.edge_count);
    return false;
}

/**
 * @brief Emit a known frame through the IR emitter and check the receiver picks it up.
 *
 * Validates the emitter, the receiver, the 38 kHz carrier and both RMT channels in one
 * shot, with no remote control and nobody present. Both burst polarities are tried,
 * because polarity depends on how the emitter is driven and how the receiver reports a
 * burst - trying only one would produce a false negative. Requires the emitter to be
 * optically coupled to the receiver (same module, or pointed at each other).
 */
static void loopback_test(void)
{
    ESP_LOGI(TAG, "loopback: up to %d attempts, 1.5 s apart - the emitter must be able to "
                  "illuminate the receiver (point them at each other, or use a reflector)",
             LOOPBACK_ATTEMPTS);

    for (int attempt = 1; attempt <= LOOPBACK_ATTEMPTS; attempt++) {
        if (loopback_attempt(0)) {
            ESP_LOGI(TAG, "loopback PASS on attempt %d (burst reported as a low level)", attempt);
            return;
        }
        if (loopback_attempt(1)) {
            ESP_LOGI(TAG, "loopback PASS on attempt %d (burst reported as a high level)", attempt);
            return;
        }
        ESP_LOGW(TAG, "loopback attempt %d/%d: no matching frame returned", attempt,
                 LOOPBACK_ATTEMPTS);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    ESP_LOGE(TAG, "loopback FAILED after %d attempts. The receive path is known good (a real "
                  "remote decodes), so the fault is on the emitter side: verify the IR LED is "
                  "driven from GPIO %d through a transistor/MOSFET and that it can actually "
                  "illuminate the receiver",
             LOOPBACK_ATTEMPTS, (int)board_pins()->ir_tx);
}
#endif /* CONFIG_APP_BRINGUP_IR_LOOPBACK */

#if CONFIG_APP_BRINGUP_IR_TX_BLINK
/* ir_frame_t::durations holds uint16_t microseconds, so a single edge cannot exceed
 * 65535 us. One 50 ms on-pulse per round plus a task delay for the off gap gives a
 * clean ~2 Hz blink; trying to store a 200 ms half-period overflows the field. */
#define BLINK_ON_US  50000 /* 50 ms on-pulse */
#define BLINK_OFF_MS 450   /* LED-off gap between pulses -> ~2 Hz */
#define BLINK_ROUNDS 20    /* ~10 s total, long enough to fetch a camera */

/**
 * @brief Alternate the emitter pin at ~2 Hz using the normal 38 kHz carrier.
 *
 * This is the ground-truth test for "is GPIO5 actually driving my LED?". The blinking is
 * unmistakable through a phone camera, and it needs no optics, no receiver and no remote.
 * A steady glow means the visible LED is a power indicator, not the emitter.
 *
 * The carrier is deliberately left ON: an earlier version drove the pin with plain DC and
 * tripped the brownout detector (`E BOD: Brownout detector was triggered`), which resets
 * the chip. A continuous DC pulse draws far more average current than a 33 % duty carrier,
 * so it exposes a weak emitter supply - see CHECKLIST.md milestone M4.
 */
static void emitter_blink_test(void)
{
    static ir_frame_t frame; /* 2 KB: keep it out of the task stack */

    ir_tx_params_t params = IR_TX_PARAMS_DEFAULT(); /* keep the 38 kHz carrier */
    params.repeats = 0;

    frame.start_level = 1; /* LED on */
    frame.edge_count = 1;
    frame.durations[0] = BLINK_ON_US;

    ESP_LOGI(TAG, "emitter blink: GPIO %d at ~2 Hz for ~%d s - the IR LED must BLINK under a "
                  "phone camera (a steady glow means it is not driven by this pin)",
             (int)board_pins()->ir_tx, (BLINK_ROUNDS * (BLINK_ON_US / 1000 + BLINK_OFF_MS)) / 1000);

    for (int i = 0; i < BLINK_ROUNDS; i++) {
        if (ir_playback_send(&frame, &params) != ESP_OK) {
            ESP_LOGW(TAG, "emitter blink: transmit failed");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(BLINK_OFF_MS)); /* LED off */
    }
}
#endif /* CONFIG_APP_BRINGUP_IR_TX_BLINK */

#if CONFIG_APP_BRINGUP_STORE_TEST
#define STORE_TEST_BUF_SIZE 4096

/* A distinctive 12-edge frame; only the bytes matter for the round trip. */
static const uint16_t s_store_test_pattern[] = {
    9000, 4500, 560, 560, 560, 1690, 560, 1690, 560, 560, 560, 1690,
};

static uint8_t s_bundle[STORE_TEST_BUF_SIZE];
static size_t s_bundle_len;
static size_t s_bundle_pos;

static esp_err_t bundle_capture(const void *data, size_t len, void *ctx)
{
    (void)ctx;
    if (s_bundle_len + len > sizeof(s_bundle)) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_bundle + s_bundle_len, data, len);
    s_bundle_len += len;
    return ESP_OK;
}

static esp_err_t bundle_replay(void *data, size_t len, void *ctx)
{
    (void)ctx;
    if (s_bundle_pos + len > s_bundle_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(data, s_bundle + s_bundle_pos, len);
    s_bundle_pos += len;
    return ESP_OK;
}

/**
 * @brief Exercise NVS + SPIFFS + export/import and verify the data comes back intact.
 *
 * Destructive by design: it begins with a factory reset so the result is deterministic.
 */
static void store_test(void)
{
    size_t profiles = 0;
    size_t commands = 0;
    size_t used = 0;

    ir_frame_t frame = { 0 };
    frame.start_level = 0;
    frame.edge_count = sizeof(s_store_test_pattern) / sizeof(s_store_test_pattern[0]);
    for (size_t i = 0; i < frame.edge_count; i++) {
        frame.durations[i] = s_store_test_pattern[i];
    }

    ir_tx_params_t params = IR_TX_PARAMS_DEFAULT();
    params.repeats = 2;
    params.repeat_gap_ms = 40;
    params.carrier_hz = 36000;

    ESP_LOGI(TAG, "store test: begin");
    ESP_ERROR_CHECK(ir_store_stats(&profiles, &commands, &used));
    ESP_LOGI(TAG, "store test: starting with %u profile(s), %u command(s), %u bytes used",
             (unsigned)profiles, (unsigned)commands, (unsigned)used);

    ESP_ERROR_CHECK(ir_store_factory_reset());

    uint8_t profile_id = 0;
    ESP_ERROR_CHECK(ir_store_profile_create("Bench", &profile_id));

    uint8_t button_id = 0;
    ESP_ERROR_CHECK(ir_store_button_save(profile_id, "Power", &frame, &params, &button_id));

    ir_button_meta_t button = { 0 };
    ESP_ERROR_CHECK(ir_store_button_get(profile_id, button_id, &button));

    ir_hotkey_t hotkey = { 0 };
    hotkey.profile_id = profile_id;
    hotkey.step_count = 2;
    hotkey.steps[0].command_id = button.command_id;
    hotkey.steps[0].delay_ms = 0;
    hotkey.steps[1].command_id = button.command_id;
    hotkey.steps[1].delay_ms = 250;
    hotkey.steps[1].repeats = 1;
    ESP_ERROR_CHECK(ir_store_hotkey_set(&hotkey));

    ESP_LOGI(TAG, "store test: created profile %u, button %u, command %" PRIu32,
             (unsigned)profile_id, (unsigned)button_id, button.command_id);

    s_bundle_len = 0;
    ESP_ERROR_CHECK(ir_store_export(bundle_capture, NULL));
    ESP_LOGI(TAG, "store test: exported %u bytes", (unsigned)s_bundle_len);

    /* Wipe, then rebuild everything from the bundle. */
    ESP_ERROR_CHECK(ir_store_factory_reset());
    ESP_ERROR_CHECK(ir_store_stats(&profiles, &commands, &used));
    if (profiles != 0 || commands != 0) {
        ESP_LOGE(TAG, "store test: FAIL - factory reset left %u profile(s), %u command(s)",
                 (unsigned)profiles, (unsigned)commands);
        return;
    }

    s_bundle_pos = 0;
    ESP_ERROR_CHECK(ir_store_import(bundle_replay, NULL));

    ir_profile_meta_t check_profile = { 0 };
    if (ir_store_profile_get(profile_id, &check_profile) != ESP_OK) {
        ESP_LOGE(TAG, "store test: FAIL - profile did not survive the round trip");
        return;
    }
    if (check_profile.button_count != 1) {
        ESP_LOGE(TAG, "store test: FAIL - profile reports %u button(s)",
                 (unsigned)check_profile.button_count);
        return;
    }

    ir_button_meta_t check_button = { 0 };
    if (ir_store_button_get(profile_id, button_id, &check_button) != ESP_OK) {
        ESP_LOGE(TAG, "store test: FAIL - button did not survive the round trip");
        return;
    }

    ir_frame_t check_frame = { 0 };
    ir_tx_params_t check_params = { 0 };
    if (ir_store_command_load(check_button.command_id, &check_frame, &check_params) != ESP_OK) {
        ESP_LOGE(TAG, "store test: FAIL - frame did not survive the round trip");
        return;
    }
    if (check_frame.edge_count != frame.edge_count ||
        check_frame.start_level != frame.start_level ||
        memcmp(check_frame.durations, frame.durations, frame.edge_count * sizeof(uint16_t)) != 0) {
        ESP_LOGE(TAG, "store test: FAIL - frame differs after the round trip");
        return;
    }
    if (check_params.carrier_hz != params.carrier_hz || check_params.repeats != params.repeats ||
        check_params.repeat_gap_ms != params.repeat_gap_ms ||
        check_params.duty_percent != params.duty_percent) {
        ESP_LOGE(TAG, "store test: FAIL - playback parameters differ after the round trip");
        return;
    }

    ir_hotkey_t check_hotkey = { 0 };
    if (ir_store_hotkey_get(&check_hotkey) != ESP_OK || check_hotkey.step_count != 2 ||
        check_hotkey.profile_id != profile_id || check_hotkey.steps[1].delay_ms != 250 ||
        check_hotkey.steps[1].repeats != 1) {
        ESP_LOGE(TAG, "store test: FAIL - hotkey did not survive the round trip");
        return;
    }

    ESP_LOGI(TAG, "store test: PASS - profile, button, %u-edge frame, playback parameters and a "
                  "2-step hotkey all round-tripped through export/import",
             (unsigned)frame.edge_count);
}
#endif /* CONFIG_APP_BRINGUP_STORE_TEST */

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C3 IR remote controller booting (IDF %s)", esp_get_idf_version());

    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(led_indicator_init());
    ESP_ERROR_CHECK(ir_capture_init());
    ESP_ERROR_CHECK(ir_playback_init());
    ESP_ERROR_CHECK(ir_store_init());
    ESP_ERROR_CHECK(ble_link_init());
    ESP_ERROR_CHECK(protocol_init());

    ESP_LOGI(TAG, "dashboard service UUID: %s (device \"%s\")", ble_link_service_uuid_str(),
             ble_link_device_name());

    /* The protocol layer drives the LED from the link state, so settle into advertising. */
    ESP_ERROR_CHECK(led_indicator_set_state(LED_STATE_ADVERTISING));

#if CONFIG_APP_BRINGUP_IR_LEARN || CONFIG_APP_BRINGUP_IR_LOOPBACK
    /* Registered before any test runs. An earlier version registered the callback after
     * the loopback had already executed, so the callback was still NULL and the loopback
     * reported "receiver saw nothing" every single time even when the receiver was
     * picking the emitter up perfectly - a pure false negative. */
    ESP_ERROR_CHECK(ir_capture_set_callback(bringup_report, NULL));
#endif

    /* Visual bring-up: three quick flashes before settling into a state. */
    ESP_ERROR_CHECK(led_indicator_set_state(LED_STATE_BOOT));
    vTaskDelay(pdMS_TO_TICKS(800));

#if CONFIG_APP_BRINGUP_STORE_TEST
    store_test();
#endif

#if CONFIG_APP_BRINGUP_IR_TX_BLINK
    emitter_blink_test();
#endif

#if CONFIG_APP_BRINGUP_IR_LOOPBACK
    loopback_test();
#endif

#if CONFIG_APP_BRINGUP_IR_LEARN
    ESP_LOGI(TAG, "bring-up: listening for IR for %d ms - point a remote at the receiver",
             CONFIG_APP_BRINGUP_IR_LEARN_WINDOW_MS);
    ESP_ERROR_CHECK(led_indicator_set_state(LED_STATE_LEARNING));
    ESP_ERROR_CHECK(ir_capture_learn_start(CONFIG_APP_BRINGUP_IR_LEARN_WINDOW_MS));
#else
    ESP_ERROR_CHECK(led_indicator_set_state(LED_STATE_ADVERTISING));
#endif

    ESP_LOGI(TAG, "boot complete");
}
