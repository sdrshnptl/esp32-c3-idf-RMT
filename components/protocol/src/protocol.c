/*
 * protocol — JSON-RPC over the BLE link.
 *
 * Request : {"id":17,"cmd":"profile.list","args":{...}}
 * Response: {"id":17,"ok":true,"data":{...}}
 *           {"id":17,"ok":false,"err":{"code":"E_...","msg":"..."}}
 * Event   : {"evt":"button.learned","data":{...}}
 *
 * Runs on the BLE link task (requests) and the IR capture task (learn callbacks). Both are
 * ordinary tasks, so blocking on NVS/SPIFFS/IR playback here is safe - it never stalls the
 * NimBLE host task.
 */

#include "protocol.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "ble_link.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "ir_capture.h"
#include "ir_playback.h"
#include "ir_store.h"
#include "led_indicator.h"
#include "sdkconfig.h"

static const char *TAG = "protocol";

/** @brief Sum of every level in a frame, in microseconds. */
static uint32_t frame_total_us(const ir_frame_t *frame)
{
    uint32_t total = 0;
    for (uint16_t i = 0; i < frame->edge_count; i++) {
        total += frame->durations[i];
    }
    return total;
}

/* ---- learn state --------------------------------------------------------- */

typedef struct {
    bool active;
    uint8_t profile_id;
    char name[IR_STORE_NAME_LEN];
} learn_state_t;

static learn_state_t s_learn;

/* ---- scratch buffers (static: no heap in the request path) --------------- */

static char s_request[CONFIG_PROTOCOL_MAX_REQUEST + 1];
static ir_profile_meta_t s_profiles[CONFIG_IR_STORE_MAX_PROFILES];
static ir_button_meta_t s_buttons[CONFIG_IR_STORE_MAX_BUTTONS];

/* ---- JSON helpers -------------------------------------------------------- */

static const cJSON *json_item(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static int json_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *item = json_item(obj, key);
    return cJSON_IsNumber(item) ? (int)item->valuedouble : fallback;
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *item = json_item(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/** @brief Print @p root, send it on RSP and free it. */
static void send_root(cJSON *root)
{
    if (root == NULL) {
        ESP_LOGE(TAG, "could not allocate a response");
        return;
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (text == NULL) {
        ESP_LOGE(TAG, "could not serialise a response");
        return;
    }

    const size_t len = strlen(text);
    if (len > CONFIG_PROTOCOL_MAX_RESPONSE) {
        ESP_LOGW(TAG, "response of %u bytes exceeds the %d byte limit", (unsigned)len,
                 CONFIG_PROTOCOL_MAX_RESPONSE);
        cJSON_free(text);

        cJSON *fallback = cJSON_CreateObject();
        if (fallback != NULL) {
            cJSON_AddNumberToObject(fallback, "id", 0);
            cJSON_AddBoolToObject(fallback, "ok", false);
            cJSON *err = cJSON_AddObjectToObject(fallback, "err");
            cJSON_AddStringToObject(err, "code", "E_TOO_LARGE");
            cJSON_AddStringToObject(err, "msg", "response exceeds the configured limit");
            text = cJSON_PrintUnformatted(fallback);
            cJSON_Delete(fallback);
        }
        if (text == NULL) {
            return;
        }
    }

    const esp_err_t err = ble_link_notify((const uint8_t *)text, strlen(text));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "notify failed: %s", esp_err_to_name(err));
    }
    cJSON_free(text);
}

static void send_ok(const cJSON *id, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(data);
        return;
    }
    cJSON_AddItemToObject(root, "id", cJSON_Duplicate(id, true));
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "data", data != NULL ? data : cJSON_CreateObject());
    send_root(root);
}

static void send_error(const cJSON *id, const char *code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddItemToObject(root, "id", cJSON_Duplicate(id, true));
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON *err = cJSON_AddObjectToObject(root, "err");
    cJSON_AddStringToObject(err, "code", code);
    cJSON_AddStringToObject(err, "msg", msg);
    send_root(root);
}

esp_err_t protocol_emit_event(const char *name, void *data)
{
    ESP_RETURN_ON_FALSE(name != NULL, ESP_ERR_INVALID_ARG, TAG, "event needs a name");

    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, TAG, "out of memory for the event");

    cJSON_AddStringToObject(root, "evt", name);
    cJSON_AddItemToObject(root, "data", (cJSON *)data != NULL ? (cJSON *)data : cJSON_CreateObject());
    send_root(root);
    return ESP_OK;
}

/* ---- sys ----------------------------------------------------------------- */

static void cmd_sys_info(const cJSON *id)
{
    const esp_app_desc_t *app = esp_app_get_description();

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        send_error(id, "E_NO_MEM", "out of memory");
        return;
    }
    cJSON_AddNumberToObject(data, "schema", PROTOCOL_SCHEMA_VERSION);
    cJSON_AddStringToObject(data, "app", app != NULL ? app->version : "unknown");
    cJSON_AddStringToObject(data, "project", app != NULL ? app->project_name : "unknown");
    cJSON_AddStringToObject(data, "idf", esp_get_idf_version());
    cJSON_AddStringToObject(data, "device", ble_link_device_name());
    cJSON_AddNumberToObject(data, "mtu", ble_link_mtu());
    cJSON_AddBoolToObject(data, "connected", ble_link_is_connected());
    cJSON_AddNumberToObject(data, "uptimeMs", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(data, "freeHeap", (double)esp_get_free_heap_size());

    cJSON *pins = cJSON_AddObjectToObject(data, "pins");
    cJSON_AddNumberToObject(pins, "irRx", (int)board_pins()->ir_rx);
    cJSON_AddNumberToObject(pins, "irTx", (int)board_pins()->ir_tx);
    cJSON_AddNumberToObject(pins, "hotkey", (int)board_pins()->hotkey);

    send_ok(id, data);
}

static void cmd_sys_stats(const cJSON *id)
{
    size_t profiles = 0;
    size_t commands = 0;
    size_t bytes = 0;
    const esp_err_t err = ir_store_stats(&profiles, &commands, &bytes);
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        send_error(id, "E_NO_MEM", "out of memory");
        return;
    }
    cJSON_AddNumberToObject(data, "profiles", (double)profiles);
    cJSON_AddNumberToObject(data, "commands", (double)commands);
    cJSON_AddNumberToObject(data, "storageBytes", (double)bytes);
    cJSON_AddNumberToObject(data, "maxProfiles", CONFIG_IR_STORE_MAX_PROFILES);
    cJSON_AddNumberToObject(data, "maxButtons", CONFIG_IR_STORE_MAX_BUTTONS);
    cJSON_AddNumberToObject(data, "maxHotkeySteps", IR_STORE_MAX_HOTKEY_STEPS);
    send_ok(id, data);
}

static void cmd_sys_factory_reset(const cJSON *id)
{
    const esp_err_t err = ir_store_factory_reset();
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "reset", true);
    send_ok(id, data);
}

/* ---- profiles ------------------------------------------------------------ */

static void cmd_profile_list(const cJSON *id)
{
    size_t count = 0;
    const esp_err_t err = ir_store_profile_list(s_profiles, CONFIG_IR_STORE_MAX_PROFILES, &count);
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }

    cJSON *data = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(data, "profiles");
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", s_profiles[i].id);
        cJSON_AddStringToObject(item, "name", s_profiles[i].name);
        cJSON_AddNumberToObject(item, "buttons", s_profiles[i].button_count);
        cJSON_AddItemToArray(array, item);
    }
    send_ok(id, data);
}

static void cmd_profile_create(const cJSON *args, const cJSON *id)
{
    const char *name = json_str(args, "name");
    if (name == NULL || name[0] == '\0') {
        send_error(id, "E_INVALID", "name is required");
        return;
    }

    uint8_t profile_id = 0;
    const esp_err_t err = ir_store_profile_create(name, &profile_id);
    if (err == ESP_ERR_NO_MEM) {
        send_error(id, "E_NO_SPACE", "no free profile slot");
        return;
    }
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "id", profile_id);
    cJSON_AddStringToObject(data, "name", name);
    send_ok(id, data);
}

static void cmd_profile_rename(const cJSON *args, const cJSON *id)
{
    const char *name = json_str(args, "name");
    const int profile_id = json_int(args, "id", 0);
    if (name == NULL || profile_id <= 0) {
        send_error(id, "E_INVALID", "id and name are required");
        return;
    }

    const esp_err_t err = ir_store_profile_rename((uint8_t)profile_id, name);
    if (err == ESP_ERR_NOT_FOUND) {
        send_error(id, "E_NO_PROFILE", "no such profile");
        return;
    }
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }
    send_ok(id, cJSON_CreateObject());
}

static void cmd_profile_delete(const cJSON *args, const cJSON *id)
{
    const int profile_id = json_int(args, "id", 0);
    if (profile_id <= 0) {
        send_error(id, "E_INVALID", "id is required");
        return;
    }

    const esp_err_t err = ir_store_profile_delete((uint8_t)profile_id);
    if (err == ESP_ERR_NOT_FOUND) {
        send_error(id, "E_NO_PROFILE", "no such profile");
        return;
    }
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }
    send_ok(id, cJSON_CreateObject());
}

/* ---- buttons ------------------------------------------------------------- */

static void cmd_button_list(const cJSON *args, const cJSON *id)
{
    const int profile_id = json_int(args, "profileId", 0);
    if (profile_id <= 0) {
        send_error(id, "E_INVALID", "profileId is required");
        return;
    }

    size_t count = 0;
    const esp_err_t err = ir_store_button_list((uint8_t)profile_id, s_buttons,
                                              CONFIG_IR_STORE_MAX_BUTTONS, &count);
    if (err == ESP_ERR_NOT_FOUND) {
        send_error(id, "E_NO_PROFILE", "no such profile");
        return;
    }
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }

    cJSON *data = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(data, "buttons");
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", s_buttons[i].id);
        cJSON_AddStringToObject(item, "name", s_buttons[i].name);
        cJSON_AddNumberToObject(item, "commandId", (double)s_buttons[i].command_id);
        cJSON_AddItemToArray(array, item);
    }
    send_ok(id, data);
}

static void cmd_button_rename(const cJSON *args, const cJSON *id)
{
    const char *name = json_str(args, "name");
    const int profile_id = json_int(args, "profileId", 0);
    const int button_id = json_int(args, "buttonId", 0);
    if (name == NULL || profile_id <= 0 || button_id <= 0) {
        send_error(id, "E_INVALID", "profileId, buttonId and name are required");
        return;
    }

    const esp_err_t err = ir_store_button_rename((uint8_t)profile_id, (uint8_t)button_id, name);
    if (err == ESP_ERR_NOT_FOUND) {
        send_error(id, "E_NO_BUTTON", "no such button");
        return;
    }
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }
    send_ok(id, cJSON_CreateObject());
}

static void cmd_button_delete(const cJSON *args, const cJSON *id)
{
    const int profile_id = json_int(args, "profileId", 0);
    const int button_id = json_int(args, "buttonId", 0);
    if (profile_id <= 0 || button_id <= 0) {
        send_error(id, "E_INVALID", "profileId and buttonId are required");
        return;
    }

    const esp_err_t err = ir_store_button_delete((uint8_t)profile_id, (uint8_t)button_id);
    if (err == ESP_ERR_NOT_FOUND) {
        send_error(id, "E_NO_BUTTON", "no such button");
        return;
    }
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }
    send_ok(id, cJSON_CreateObject());
}

/** @brief Load the frame behind a (profileId, buttonId) pair. */
static esp_err_t load_button_frame(const cJSON *args, ir_frame_t *out_frame,
                                  ir_tx_params_t *out_params, ir_button_meta_t *out_button)
{
    const int profile_id = json_int(args, "profileId", 0);
    const int button_id = json_int(args, "buttonId", 0);
    ESP_RETURN_ON_FALSE(profile_id > 0 && button_id > 0, ESP_ERR_INVALID_ARG, TAG, "invalid ids");

    ESP_RETURN_ON_ERROR(ir_store_button_get((uint8_t)profile_id, (uint8_t)button_id, out_button),
                        TAG, "button lookup failed");
    ESP_RETURN_ON_FALSE(out_button->command_id != 0, ESP_ERR_NOT_FOUND, TAG,
                        "button has no learned frame");
    return ir_store_command_load(out_button->command_id, out_frame, out_params);
}

static void cmd_button_waveform(const cJSON *args, const cJSON *id)
{
    static ir_frame_t frame; /* ~1 KB, so static */
    ir_tx_params_t params = { 0 };
    ir_button_meta_t button = { 0 };

    const esp_err_t err = load_button_frame(args, &frame, &params, &button);
    if (err == ESP_ERR_INVALID_ARG) {
        send_error(id, "E_INVALID", "profileId and buttonId are required");
        return;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        send_error(id, "E_NO_COMMAND", "button has no learned frame");
        return;
    }
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }

    const size_t limit = CONFIG_PROTOCOL_WAVEFORM_MAX_EDGES;
    const size_t edges = (frame.edge_count < limit) ? frame.edge_count : limit;

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "commandId", (double)button.command_id);
    cJSON_AddStringToObject(data, "name", button.name);
    cJSON_AddNumberToObject(data, "edges", frame.edge_count);
    cJSON_AddNumberToObject(data, "startLevel", frame.start_level);
    cJSON_AddBoolToObject(data, "truncated", frame.edge_count > edges);
    cJSON_AddNumberToObject(data, "carrierHz", (double)params.carrier_hz);
    cJSON_AddNumberToObject(data, "repeats", params.repeats);

    cJSON *array = cJSON_AddArrayToObject(data, "durations");
    for (size_t i = 0; i < edges; i++) {
        cJSON_AddItemToArray(array, cJSON_CreateNumber(frame.durations[i]));
    }
    send_ok(id, data);
}

static void cmd_button_play(const cJSON *args, const cJSON *id)
{
    static ir_frame_t frame;
    ir_tx_params_t params = { 0 };
    ir_button_meta_t button = { 0 };

    const esp_err_t err = load_button_frame(args, &frame, &params, &button);
    if (err == ESP_ERR_INVALID_ARG) {
        send_error(id, "E_INVALID", "profileId and buttonId are required");
        return;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        send_error(id, "E_NO_COMMAND", "button has no learned frame");
        return;
    }
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }

    const int repeats = json_int(args, "repeats", params.repeats);
    params.repeats = (repeats >= 0 && repeats <= 10) ? (uint8_t)repeats : params.repeats;

    if (led_indicator_pulse(LED_PULSE_IR_FRAME) != ESP_OK) {
        ESP_LOGW(TAG, "could not pulse the LED");
    }

    const esp_err_t play_err = ir_playback_send(&frame, &params);
    if (play_err != ESP_OK) {
        send_error(id, "E_PLAYBACK", esp_err_to_name(play_err));
        return;
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "commandId", (double)button.command_id);
    cJSON_AddNumberToObject(data, "edges", frame.edge_count);
    cJSON_AddNumberToObject(data, "repeats", params.repeats);
    send_ok(id, data);
}

/* ---- learn --------------------------------------------------------------- */

static void learn_finish(const ir_frame_t *frame)
{
    ir_tx_params_t params = IR_TX_PARAMS_DEFAULT();
    uint8_t button_id = 0;

    const esp_err_t err = ir_store_button_save(s_learn.profile_id, s_learn.name, frame, &params,
                                              &button_id);
    s_learn.active = false;
    if (ir_capture_learn_cancel() != ESP_OK) {
        ESP_LOGW(TAG, "could not disarm the receiver");
    }
    if (led_indicator_set_state(ble_link_is_connected() ? LED_STATE_CONNECTED
                                                        : LED_STATE_ADVERTISING) != ESP_OK) {
        ESP_LOGW(TAG, "could not restore the LED state");
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "learn failed for \"%s\": %s", s_learn.name, esp_err_to_name(err));
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "reason", esp_err_to_name(err));
        protocol_emit_event("button.learn_failed", data);
        return;
    }

    /* Report the stored frame so the dashboard can draw it without a second round trip. */
    ir_button_meta_t button = { 0 };
    if (ir_store_button_get(s_learn.profile_id, button_id, &button) != ESP_OK) {
        ESP_LOGW(TAG, "stored button %u could not be read back", (unsigned)button_id);
    }

    const size_t limit = CONFIG_PROTOCOL_WAVEFORM_MAX_EDGES;
    const size_t edges = (frame->edge_count < limit) ? frame->edge_count : limit;

    ESP_LOGI(TAG, "learned \"%s\" -> profile %u button %u (%u edges, %u us)", s_learn.name,
             (unsigned)s_learn.profile_id, (unsigned)button_id, (unsigned)frame->edge_count,
             (unsigned)frame_total_us(frame));

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "profileId", s_learn.profile_id);
    cJSON_AddNumberToObject(data, "buttonId", button_id);
    cJSON_AddNumberToObject(data, "commandId", (double)button.command_id);
    cJSON_AddStringToObject(data, "name", s_learn.name);
    cJSON_AddNumberToObject(data, "edges", frame->edge_count);
    cJSON_AddNumberToObject(data, "startLevel", frame->start_level);
    cJSON_AddBoolToObject(data, "truncated", frame->edge_count > edges);
    cJSON *array = cJSON_AddArrayToObject(data, "durations");
    for (size_t i = 0; i < edges; i++) {
        cJSON_AddItemToArray(array, cJSON_CreateNumber(frame->durations[i]));
    }
    protocol_emit_event("button.learned", data);
}

static void cmd_button_learn(const cJSON *args, const cJSON *id)
{
    const int profile_id = json_int(args, "profileId", 0);
    const char *name = json_str(args, "name");
    const int timeout_ms = json_int(args, "timeoutMs", CONFIG_PROTOCOL_LEARN_TIMEOUT_MS);

    if (profile_id <= 0 || name == NULL || name[0] == '\0') {
        send_error(id, "E_INVALID", "profileId and name are required");
        return;
    }
    if (s_learn.active) {
        send_error(id, "E_BUSY", "a learn session is already running");
        return;
    }

    ir_profile_meta_t profile = { 0 };
    const esp_err_t err = ir_store_profile_get((uint8_t)profile_id, &profile);
    if (err == ESP_ERR_NOT_FOUND) {
        send_error(id, "E_NO_PROFILE", "no such profile");
        return;
    }
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }

    s_learn.active = true;
    s_learn.profile_id = (uint8_t)profile_id;
    snprintf(s_learn.name, sizeof(s_learn.name), "%s", name);

    if (led_indicator_set_state(LED_STATE_LEARNING) != ESP_OK) {
        ESP_LOGW(TAG, "could not show the learning indication");
    }
    const esp_err_t arm = ir_capture_learn_start((uint32_t)timeout_ms);
    if (arm != ESP_OK) {
        s_learn.active = false;
        send_error(id, "E_CAPTURE", esp_err_to_name(arm));
        return;
    }

    ESP_LOGI(TAG, "listening for \"%s\" into profile %u (timeout %d ms)", s_learn.name,
             (unsigned)s_learn.profile_id, timeout_ms);

    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "learning", true);
    cJSON_AddNumberToObject(data, "timeoutMs", timeout_ms);
    send_ok(id, data);
}

static void cmd_button_learn_cancel(const cJSON *id)
{
    s_learn.active = false;
    const esp_err_t err = ir_capture_learn_cancel();
    if (err != ESP_OK) {
        send_error(id, "E_CAPTURE", esp_err_to_name(err));
        return;
    }
    if (led_indicator_set_state(ble_link_is_connected() ? LED_STATE_CONNECTED
                                                        : LED_STATE_ADVERTISING) != ESP_OK) {
        ESP_LOGW(TAG, "could not restore the LED state");
    }
    send_ok(id, cJSON_CreateObject());
}

/* ---- IR ------------------------------------------------------------------ */

static void cmd_ir_play(const cJSON *args, const cJSON *id)
{
    static ir_frame_t frame;
    ir_tx_params_t params = { 0 };

    const uint32_t command_id = (uint32_t)json_int(args, "commandId", 0);
    if (command_id == 0) {
        send_error(id, "E_INVALID", "commandId is required");
        return;
    }

    const esp_err_t err = ir_store_command_load(command_id, &frame, &params);
    if (err == ESP_ERR_NOT_FOUND) {
        send_error(id, "E_NO_COMMAND", "no such command");
        return;
    }
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }

    const int repeats = json_int(args, "repeats", params.repeats);
    params.repeats = (repeats >= 0 && repeats <= 10) ? (uint8_t)repeats : params.repeats;

    if (led_indicator_pulse(LED_PULSE_IR_FRAME) != ESP_OK) {
        ESP_LOGW(TAG, "could not pulse the LED");
    }
    const esp_err_t play_err = ir_playback_send(&frame, &params);
    if (play_err != ESP_OK) {
        send_error(id, "E_PLAYBACK", esp_err_to_name(play_err));
        return;
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "commandId", (double)command_id);
    cJSON_AddNumberToObject(data, "edges", frame.edge_count);
    send_ok(id, data);
}

/* ---- hotkey -------------------------------------------------------------- */

static void cmd_hotkey_get(const cJSON *id)
{
    ir_hotkey_t hotkey = { 0 };
    const esp_err_t err = ir_store_hotkey_get(&hotkey);
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "configured", hotkey.step_count > 0);
    cJSON_AddNumberToObject(data, "profileId", hotkey.profile_id);
    cJSON_AddNumberToObject(data, "maxSteps", IR_STORE_MAX_HOTKEY_STEPS);
    cJSON *array = cJSON_AddArrayToObject(data, "steps");
    for (size_t i = 0; i < hotkey.step_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "commandId", (double)hotkey.steps[i].command_id);
        cJSON_AddNumberToObject(item, "delayMs", hotkey.steps[i].delay_ms);
        cJSON_AddNumberToObject(item, "repeats", hotkey.steps[i].repeats);
        cJSON_AddItemToArray(array, item);
    }
    send_ok(id, data);
}

static void cmd_hotkey_set(const cJSON *args, const cJSON *id)
{
    const int profile_id = json_int(args, "profileId", 0);
    const cJSON *steps = json_item(args, "steps");
    if (profile_id <= 0 || !cJSON_IsArray(steps)) {
        send_error(id, "E_INVALID", "profileId and a steps array are required");
        return;
    }

    const int count = cJSON_GetArraySize(steps);
    if (count < 0 || count > IR_STORE_MAX_HOTKEY_STEPS) {
        send_error(id, "E_INVALID", "at most 8 hotkey steps are supported");
        return;
    }

    ir_hotkey_t hotkey = { 0 };
    hotkey.profile_id = (uint8_t)profile_id;
    hotkey.step_count = (uint8_t)count;

    for (int i = 0; i < count; i++) {
        const cJSON *item = cJSON_GetArrayItem(steps, i);
        const int command_id = json_int(item, "commandId", 0);
        if (command_id <= 0) {
            send_error(id, "E_INVALID", "every step needs a commandId");
            return;
        }
        const int delay = json_int(item, "delayMs", 0);
        const int repeats = json_int(item, "repeats", 0);
        hotkey.steps[i].command_id = (uint32_t)command_id;
        hotkey.steps[i].delay_ms = (uint16_t)((delay >= 0 && delay <= 60000) ? delay : 0);
        hotkey.steps[i].repeats = (uint8_t)((repeats >= 0 && repeats <= 10) ? repeats : 0);
    }

    const esp_err_t err = ir_store_hotkey_set(&hotkey);
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "steps", count);
    send_ok(id, data);
}

static void cmd_hotkey_clear(const cJSON *id)
{
    const esp_err_t err = ir_store_hotkey_clear();
    if (err != ESP_OK) {
        send_error(id, "E_STORE", esp_err_to_name(err));
        return;
    }
    send_ok(id, cJSON_CreateObject());
}

/* ---- dispatch ------------------------------------------------------------ */

static bool streq(const char *a, const char *b)
{
    return a != NULL && strcmp(a, b) == 0;
}

static void dispatch(const cJSON *root, const cJSON *id, const char *cmd, const cJSON *args)
{
    (void)root;

    if (streq(cmd, "sys.info")) {
        cmd_sys_info(id);
    } else if (streq(cmd, "sys.stats")) {
        cmd_sys_stats(id);
    } else if (streq(cmd, "sys.factory_reset")) {
        cmd_sys_factory_reset(id);
    } else if (streq(cmd, "profile.list")) {
        cmd_profile_list(id);
    } else if (streq(cmd, "profile.create")) {
        cmd_profile_create(args, id);
    } else if (streq(cmd, "profile.rename")) {
        cmd_profile_rename(args, id);
    } else if (streq(cmd, "profile.delete")) {
        cmd_profile_delete(args, id);
    } else if (streq(cmd, "button.list")) {
        cmd_button_list(args, id);
    } else if (streq(cmd, "button.learn")) {
        cmd_button_learn(args, id);
    } else if (streq(cmd, "button.learn_cancel")) {
        cmd_button_learn_cancel(id);
    } else if (streq(cmd, "button.rename")) {
        cmd_button_rename(args, id);
    } else if (streq(cmd, "button.delete")) {
        cmd_button_delete(args, id);
    } else if (streq(cmd, "button.waveform")) {
        cmd_button_waveform(args, id);
    } else if (streq(cmd, "button.play")) {
        cmd_button_play(args, id);
    } else if (streq(cmd, "ir.play")) {
        cmd_ir_play(args, id);
    } else if (streq(cmd, "hotkey.get")) {
        cmd_hotkey_get(id);
    } else if (streq(cmd, "hotkey.set")) {
        cmd_hotkey_set(args, id);
    } else if (streq(cmd, "hotkey.clear")) {
        cmd_hotkey_clear(id);
    } else {
        send_error(id, "E_UNKNOWN_CMD", cmd != NULL ? cmd : "(missing \"cmd\")");
    }
}

/* ---- callbacks ----------------------------------------------------------- */

static void on_request(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;

    if (len == 0 || len > CONFIG_PROTOCOL_MAX_REQUEST) {
        ESP_LOGW(TAG, "rejecting a %u-byte request (max %d)", (unsigned)len,
                 CONFIG_PROTOCOL_MAX_REQUEST);
        cJSON *id = cJSON_CreateNumber(0);
        send_error(id, "E_TOO_LARGE", "request exceeds the configured limit");
        cJSON_Delete(id);
        return;
    }

    /* The transport delivers raw bytes; cJSON needs a terminated buffer. */
    memcpy(s_request, data, len);
    s_request[len] = '\0';

    cJSON *root = cJSON_ParseWithLength(s_request, len + 1);
    if (root == NULL) {
        ESP_LOGW(TAG, "malformed JSON request");
        cJSON *id = cJSON_CreateNumber(0);
        send_error(id, "E_PARSE", "malformed JSON");
        cJSON_Delete(id);
        return;
    }

    const cJSON *id = json_item(root, "id");
    const char *cmd = json_str(root, "cmd");
    const cJSON *args = json_item(root, "args");

    if (cmd == NULL) {
        send_error(id, "E_INVALID", "\"cmd\" is required");
    } else {
        ESP_LOGD(TAG, "cmd: %s", cmd);
        dispatch(root, id, cmd, args);
    }
    cJSON_Delete(root);
}

static void on_link_state(bool connected, void *ctx)
{
    (void)ctx;

    if (led_indicator_set_state(connected ? LED_STATE_CONNECTED : LED_STATE_ADVERTISING) != ESP_OK) {
        ESP_LOGW(TAG, "could not update the LED state");
    }
    /* An interrupted learn session must not survive a disconnect. */
    if (!connected && s_learn.active) {
        s_learn.active = false;
        if (ir_capture_learn_cancel() != ESP_OK) {
            ESP_LOGW(TAG, "could not cancel the learn session");
        }
    }

    /*
     * Deliberately no "link" event here.
     *
     * A central can only be told about a connection *after* it has connected, discovered the
     * service and subscribed to RSP - so the connect-time event is undeliverable by construction,
     * and by disconnect time there is no peer left to receive it. Sending them anyway cost five or
     * six wasted chunk writes on every connection (at the default MTU that is 16-byte chunks) and
     * ran cJSON on the NimBLE host task, whose 4 KB stack is no place for a recursive JSON printer.
     * The dashboard tracks connection state from its own GATT callbacks, which is strictly more
     * accurate anyway.
     */
}

static void on_capture(ir_capture_event_t event, const ir_frame_t *frame, void *ctx)
{
    (void)ctx;

    if (!s_learn.active) {
        return;
    }

    switch (event) {
    case IR_CAPTURE_EVENT_FRAME:
        learn_finish(frame);
        break;

    case IR_CAPTURE_EVENT_OVERFLOW: {
        ESP_LOGW(TAG, "learn: frame too large, session left armed so the user can retry");
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "reason", "frame_too_large");
        protocol_emit_event("button.learn_failed", data);
        /* Keep the session armed: the user can simply try again. */
        if (ir_capture_learn_start(CONFIG_PROTOCOL_LEARN_TIMEOUT_MS) != ESP_OK) {
            ESP_LOGW(TAG, "could not re-arm after an oversized frame");
        }
        break;
    }

    case IR_CAPTURE_EVENT_TIMEOUT:
        ESP_LOGI(TAG, "learn: timed out waiting for a frame");
        s_learn.active = false;
        if (led_indicator_set_state(ble_link_is_connected() ? LED_STATE_CONNECTED
                                                            : LED_STATE_ADVERTISING) != ESP_OK) {
            ESP_LOGW(TAG, "could not restore the LED state");
        }
        {
            cJSON *data = cJSON_CreateObject();
            cJSON_AddStringToObject(data, "reason", "timeout");
            protocol_emit_event("button.learn_failed", data);
        }
        break;

    default:
        break;
    }
}

/* ---- init ---------------------------------------------------------------- */

esp_err_t protocol_init(void)
{
    ESP_RETURN_ON_ERROR(ble_link_set_rx_handler(on_request, NULL), TAG, "register rx failed");
    ESP_RETURN_ON_ERROR(ble_link_set_conn_handler(on_link_state, NULL), TAG,
                        "register conn handler failed");
    ESP_RETURN_ON_ERROR(ir_capture_set_callback(on_capture, NULL), TAG,
                        "register capture callback failed");

    ESP_LOGI(TAG, "ready (schema %d, request <= %d bytes, response <= %d bytes)",
             PROTOCOL_SCHEMA_VERSION, CONFIG_PROTOCOL_MAX_REQUEST, CONFIG_PROTOCOL_MAX_RESPONSE);
    return ESP_OK;
}
