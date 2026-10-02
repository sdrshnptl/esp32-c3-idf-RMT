#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest profile or button name, including the terminator. */
#define IR_STORE_NAME_LEN 24

/** Maximum number of steps in the GPIO0 hotkey sequence. */
#define IR_STORE_MAX_HOTKEY_STEPS 8

/** A stored command id of 0 means "no command". */
typedef uint32_t ir_command_id_t;

/**
 * @brief Profile metadata (no frame data).
 */
typedef struct {
    uint8_t id;                            /*!< 1..255, 0 is invalid */
    char name[IR_STORE_NAME_LEN];          /*!< NUL-terminated */
    uint8_t button_count;
} ir_profile_meta_t;

/**
 * @brief Button metadata. The infrared frame itself lives in SPIFFS, keyed by command_id.
 */
typedef struct {
    uint8_t id;                            /*!< 1..255, unique within the profile */
    char name[IR_STORE_NAME_LEN];          /*!< NUL-terminated */
    ir_command_id_t command_id;            /*!< 0 when nothing has been learned yet */
} ir_button_meta_t;

/**
 * @brief One step of the hotkey sequence.
 */
typedef struct {
    ir_command_id_t command_id;
    uint16_t delay_ms;                     /*!< Added before this step is played */
    uint8_t repeats;
} ir_hotkey_step_t;

/**
 * @brief Hotkey sequence: up to IR_STORE_MAX_HOTKEY_STEPS commands from one profile.
 */
typedef struct {
    uint8_t profile_id;                    /*!< Profile the steps belong to, 0 = unset */
    uint8_t step_count;
    ir_hotkey_step_t steps[IR_STORE_MAX_HOTKEY_STEPS];
} ir_hotkey_t;

/** @brief Writer used by ir_store_export(). Must return ESP_OK or the export aborts. */
typedef esp_err_t (*ir_store_write_fn)(const void *data, size_t len, void *ctx);

/** @brief Reader used by ir_store_import(). Must fill @p len bytes or fail. */
typedef esp_err_t (*ir_store_read_fn)(void *data, size_t len, void *ctx);

/**
 * @brief Initialise NVS, mount SPIFFS and create the storage lock.
 *
 * Safe to call once; subsequent calls return ESP_ERR_INVALID_STATE.
 */
esp_err_t ir_store_init(void);

/** @brief Unmount SPIFFS, close NVS and release the lock. */
esp_err_t ir_store_deinit(void);

/* ---- Profiles ------------------------------------------------------------ */

/** @brief List all profiles into a caller-provided array. */
esp_err_t ir_store_profile_list(ir_profile_meta_t *out, size_t capacity, size_t *count);

/** @brief Fetch one profile. */
esp_err_t ir_store_profile_get(uint8_t profile_id, ir_profile_meta_t *out);

/** @brief Create a profile and return its new id. */
esp_err_t ir_store_profile_create(const char *name, uint8_t *out_id);

/** @brief Rename a profile. */
esp_err_t ir_store_profile_rename(uint8_t profile_id, const char *name);

/** @brief Delete a profile, its buttons and all of their stored frames. */
esp_err_t ir_store_profile_delete(uint8_t profile_id);

/* ---- Buttons ------------------------------------------------------------- */

/** @brief List the buttons of one profile. */
esp_err_t ir_store_button_list(uint8_t profile_id, ir_button_meta_t *out, size_t capacity,
                              size_t *count);

/** @brief Fetch one button. */
esp_err_t ir_store_button_get(uint8_t profile_id, uint8_t button_id, ir_button_meta_t *out);

/**
 * @brief Store a learned frame as a new button.
 *
 * Allocates a fresh command id, writes the frame to SPIFFS and adds the metadata.
 */
esp_err_t ir_store_button_save(uint8_t profile_id, const char *name, const ir_frame_t *frame,
                              const ir_tx_params_t *params, uint8_t *out_button_id);

/** @brief Rename a button. */
esp_err_t ir_store_button_rename(uint8_t profile_id, uint8_t button_id, const char *name);

/** @brief Delete a button and its stored frame. */
esp_err_t ir_store_button_delete(uint8_t profile_id, uint8_t button_id);

/* ---- Commands ------------------------------------------------------------ */

/** @brief Load a stored frame and its playback parameters. */
esp_err_t ir_store_command_load(ir_command_id_t command_id, ir_frame_t *out_frame,
                               ir_tx_params_t *out_params);

/* ---- Hotkey -------------------------------------------------------------- */

/** @brief Read the hotkey sequence. step_count is 0 when nothing is configured. */
esp_err_t ir_store_hotkey_get(ir_hotkey_t *out);

/** @brief Write the hotkey sequence (at most IR_STORE_MAX_HOTKEY_STEPS steps). */
esp_err_t ir_store_hotkey_set(const ir_hotkey_t *in);

/** @brief Forget the hotkey sequence. */
esp_err_t ir_store_hotkey_clear(void);

/* ---- Housekeeping -------------------------------------------------------- */

/** @brief Report how much is stored: profile count, command count and SPIFFS bytes used. */
esp_err_t ir_store_stats(size_t *out_profiles, size_t *out_commands, size_t *out_used_bytes);

/**
 * @brief Serialise everything to @p write as one self-describing bundle.
 *
 * Binary, not JSON: the transport layer decides how to frame it. The format is documented in
 * the component README and is stable across reboots of the same schema version.
 */
esp_err_t ir_store_export(ir_store_write_fn write, void *ctx);

/**
 * @brief Replace all stored data with the bundle read from @p read.
 *
 * The store is wiped first, so a failed import leaves an empty store rather than a mixture.
 */
esp_err_t ir_store_import(ir_store_read_fn read, void *ctx);

/** @brief Erase every profile, button, frame and the hotkey sequence. */
esp_err_t ir_store_factory_reset(void);

#ifdef __cplusplus
}
#endif
