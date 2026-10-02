# ir_store

Persistence for profiles, buttons, learned frames and the GPIO0 hotkey sequence.

**NVS** holds the metadata, **SPIFFS** holds the raw frames. Metadata is small and updated often
(NVS is wear-levelled and transactional); frames are larger blobs that are written once and read
whole (a filesystem suits them better).

## API

```c
#include "ir_store.h"

ESP_ERROR_CHECK(ir_store_init());

uint8_t profile_id = 0;
ESP_ERROR_CHECK(ir_store_profile_create("Living room", &profile_id));

uint8_t button_id = 0;
ir_tx_params_t params = IR_TX_PARAMS_DEFAULT();
ESP_ERROR_CHECK(ir_store_button_save(profile_id, "Power", &frame, &params, &button_id));

ir_frame_t out_frame;
ir_tx_params_t out_params;
ESP_ERROR_CHECK(ir_store_command_load(button.command_id, &out_frame, &out_params));

ir_hotkey_t hotkey = { .profile_id = profile_id, .step_count = 2, ... };
ESP_ERROR_CHECK(ir_store_hotkey_set(&hotkey));
```

All calls are serialised by a recursive mutex, so they are safe from the BLE task, the hotkey task
and the capture callback at once. Every entry point validates its arguments and returns an
`esp_err_t`; nothing allocates on the heap.

## Storage layout

| Location | Key / name | Contents |
|---|---|---|
| NVS `irstore` | `ver` | Schema version. A mismatch wipes the store, because the blobs mirror the in-memory structs |
| NVS | `nextprof`, `nextcmd` | Monotonic id counters |
| NVS | `pmeta` | Profile table |
| NVS | `but<id>` | Button table for one profile |
| NVS | `hk` | Hotkey sequence |
| SPIFFS | `/spiffs/c%08x.bin` | One learned frame per command id |

Frame file layout: a 22-byte `command_header_t` (magic, version, polarity, edge count, carrier
frequency, repeat gap, duty, repeats) followed by `edge_count` little-endian `uint16_t`
microsecond durations. The file size is validated on read and a bad magic/version is rejected rather
than guessed at.

## Limits

Defaults: 16 profiles, 32 buttons each, 8 hotkey steps. The `storage` partition (256 KB) holds
roughly 2000 learned frames.

## Export / import

`ir_store_export()` / `ir_store_import()` move the whole configuration through caller-supplied
read/write callbacks, so the same code serves a SPIFFS file, a BLE transfer or a RAM buffer.

The bundle is **field-by-field little-endian, not a struct dump**, so it stays valid even if the
internal structs change:

```
u32 magic 'IREX' | u16 schema | u8 profile_count | u32 command_count
per profile:  u8 id | u8 button_count | per button: u8 id | u32 command_id | char name[24]
              char name[24]
hotkey:       u8 profile_id | u8 step_count | per step: u32 command_id | u16 delay_ms | u8 repeats
per command:  u32 id | u32 payload_len | payload bytes (the SPIFFS file verbatim)
```

Import wipes the store **first**, so a failed import leaves an empty store rather than a mixture of
old and new data, and it leaves the id counters above every imported id so new entries cannot
collide.

## Verified

Hardware round-trip on 2026-10-02: create profile + button + hotkey, export 136 bytes, factory
reset, import, then verify the profile, button, raw 12-edge frame bytes, playback parameters and a
2-step hotkey all survived.
