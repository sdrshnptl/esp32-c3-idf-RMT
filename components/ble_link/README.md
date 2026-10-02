# ble_link

NimBLE peripheral link for the Web Bluetooth dashboard. Deliberately **protocol-agnostic**: it
moves framed byte buffers and knows nothing about JSON.

> **Why "link" and not "transport"?** NimBLE already declares `ble_transport_init()` and
> `ble_transport_deinit()` for its own HCI transport layer in `nimble/transport.h`. Using that
> prefix made the two headers collide, so this component uses `ble_link_*`.

## API

```c
#include "ble_link.h"

ESP_ERROR_CHECK(ble_link_init());
ESP_ERROR_CHECK(ble_link_set_rx_handler(on_message, NULL));
ESP_ERROR_CHECK(ble_link_set_conn_handler(on_link, NULL));
ESP_ERROR_CHECK(ble_link_notify(json, len));   /* chunked automatically */
```

| Function | Purpose |
|---|---|
| `ble_link_init()` | Start NimBLE, register the service and advertise |
| `ble_link_notify()` | Send on RSP, chunked to the negotiated MTU |
| `ble_link_notify_status()` | Send on STATUS |
| `ble_link_set_rx_handler()` | Called with each complete, reassembled message |
| `ble_link_set_conn_handler()` | Called on connect/disconnect |
| `ble_link_is_connected()` / `ble_link_mtu()` | Link state |
| `ble_link_service_uuid_str()` | For the dashboard's `requestDevice()` filter |

## GATT layout

Base UUID `a1e9xxxx-6c2b-4f1a-9d3e-b1c2d3e4f5a6`:

| Char | UUID | Properties |
|---|---|---|
| service | `a1e90000-…` | primary |
| CMD | `a1e90001-…` | Write, Write-No-Rsp |
| RSP | `a1e90002-…` | Notify |
| RAW | `a1e90003-…` | Write-No-Rsp, Notify |
| STATUS | `a1e90004-…` | Read, Notify |

## Framing

Both directions carry a 4-byte little-endian header per chunk:

```
[u16 total_len][u16 offset][payload...]
```

Inbound chunks are reassembled and must arrive in order; `offset == 0` starts a new message.
Outbound messages are split to `mtu - 3 - 4` bytes per notification.

## Design notes

- **The NimBLE host task is never blocked.** The GATT access callback only does bounded memcpy
  work and then queues the message; a dedicated link task invokes the RX handler, so the protocol
  layer may freely block on NVS, SPIFFS or IR playback.
- **Bounded static memory.** Two pool buffers plus one reassembly buffer, all
  `CONFIG_BLE_LINK_RX_MAX`. If both pool slots are busy the message is dropped with a warning
  rather than queued without limit.
- **Advertising payload budget.** A 128-bit service UUID costs 18 of the 31 available bytes, so the
  service UUID goes in the advertising packet (Web Bluetooth filters on it) and the device name in
  the scan response.
- **Security.** Open GATT for the MVP: `sm_bonding = 0`, `sm_mitm = 0`, no IO capability. Bonding
  is an integration step, not a design change — the characteristic flags sit in one table.
- **Reconnect handling.** A failed connect or a completed advertisement restarts advertising, so
  the device becomes discoverable again without intervention.

## Verified NimBLE API (ESP-IDF master)

`nimble_port_init()`, `nimble_port_freertos_init()`, `ble_gap_adv_set_fields()`,
`ble_gap_adv_rsp_set_fields()`, `ble_gap_adv_start()`, `ble_gatts_add_svcs()`,
`ble_gatts_find_chr()`, `ble_gatts_notify_custom()`, `ble_att_mtu()`,
`ble_att_set_preferred_mtu()`, `ble_hs_mbuf_to_flat()`, `os_msys_get_pkthdr()`.
