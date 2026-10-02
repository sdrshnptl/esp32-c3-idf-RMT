#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Wire the JSON-RPC layer onto the BLE link.
 *
 * Registers the RX handler, the link-state handler and the IR capture callback, so it must be
 * called after ble_link_init(), ir_capture_init() and ir_store_init().
 */
esp_err_t protocol_init(void);

/**
 * @brief Send an unsolicited event to the dashboard.
 *
 * @param name  Event name, e.g. "link" or "button.learned".
 * @param data  Object to attach as "data". Ownership is transferred.
 */
esp_err_t protocol_emit_event(const char *name, void *data);

/** @brief The request/response schema version reported by sys.info. */
#define PROTOCOL_SCHEMA_VERSION 1

#ifdef __cplusplus
}
#endif
