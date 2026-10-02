#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback for a complete, reassembled message from the dashboard.
 *
 * Runs in the link's own task, never in the NimBLE host task or an ISR, so it may block
 * (NVS, SPIFFS, IR playback) without stalling the Bluetooth stack.
 */
typedef void (*ble_link_rx_cb_t)(const uint8_t *data, size_t len, void *ctx);

/** @brief Callback for link state changes. Runs in the NimBLE host task. */
typedef void (*ble_link_conn_cb_t)(bool connected, void *ctx);

/**
 * @brief Start the NimBLE peripheral, register the GATT service and advertise.
 *
 * Requires NVS to be initialised.
 *
 * @note Named "link" rather than "transport" on purpose: NimBLE already reserves the
 *       `ble_transport_*` symbol prefix for its own HCI transport layer.
 */
esp_err_t ble_link_init(void);

/** @brief Stop advertising, drop the connection and tear the stack down. */
esp_err_t ble_link_deinit(void);

/** @brief Whether a central is currently connected. */
bool ble_link_is_connected(void);

/** @brief Negotiated ATT MTU (23 until the peer exchanges MTU). */
uint16_t ble_link_mtu(void);

/** @brief Register the handler for complete inbound messages. */
esp_err_t ble_link_set_rx_handler(ble_link_rx_cb_t cb, void *ctx);

/** @brief Register the handler for connect/disconnect. */
esp_err_t ble_link_set_conn_handler(ble_link_conn_cb_t cb, void *ctx);

/**
 * @brief Send a response message on the RSP characteristic.
 *
 * Long messages are split into MTU-sized chunks, each carrying a 4-byte header
 * ([u16 total][u16 offset], little-endian) so the peer can reassemble them.
 */
esp_err_t ble_link_notify(const uint8_t *data, size_t len);

/** @brief Send a message on the STATUS characteristic (same framing as RSP). */
esp_err_t ble_link_notify_status(const uint8_t *data, size_t len);

/**
 * @brief The 128-bit primary service UUID, as a canonical lowercase string.
 *
 * The dashboard needs the exact string for `navigator.bluetooth.requestDevice()`.
 */
const char *ble_link_service_uuid_str(void);

/** @brief Advertising name actually in use, e.g. "IR-RMT-1A2B". */
const char *ble_link_device_name(void);

#ifdef __cplusplus
}
#endif
