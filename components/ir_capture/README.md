# ir_capture

Records arbitrary infrared frames — protocol-agnostic, no NEC/RC5 knowledge required.

## API

```c
#include "ir_capture.h"

ESP_ERROR_CHECK(ir_capture_init());
ESP_ERROR_CHECK(ir_capture_set_callback(on_frame, NULL));
ESP_ERROR_CHECK(ir_capture_learn_start(15000));
```

| Function | Purpose |
|---|---|
| `ir_capture_init()` | Create the RMT RX channel and start the capture task |
| `ir_capture_set_callback()` | Register the event callback (NULL clears it) |
| `ir_capture_learn_start()` | Arm the receiver; `window_ms` is an **inactivity** timeout |
| `ir_capture_learn_cancel()` | Disarm without reporting an event |
| `ir_capture_is_learning()` | Whether the receiver is armed |
| `ir_capture_frames_match()` | Tolerance comparison used for de-duplication |
| `ir_capture_deinit()` | Release the channel and stop the task |

## Events

`IR_CAPTURE_EVENT_FRAME`, `_REPEAT`, `_TIMEOUT`, `_OVERFLOW`. The callback receives a pointer to a
component-owned `ir_frame_t` that is reused on the next capture — copy it to keep it.

Callbacks run in the capture task (or the esp_timer task for `_TIMEOUT`), never in an ISR, so NVS,
BLE and LED calls are safe from them.

## How it works

- **RMT RX at 1 MHz** → one RMT tick is exactly 1 µs, so durations map 1:1 and the 15-bit
  duration field caps any single level at 32.767 ms.
- **Bounce buffer**: the driver accumulates symbols into a ~2 KB static buffer
  (`IR_FRAME_MAX_SYMBOLS`) across ping-pong interrupts, and reports the whole frame once the line
  goes idle for `CONFIG_IR_CAPTURE_FRAME_GAP_MS`.
- **ISR does almost nothing** — it records `num_symbols` + `is_last` and notifies the task.
  Symbol→duration conversion happens in task context.
- **Overflow is explicit**: `en_partial_rx` is enabled so that a frame longer than the buffer
  arrives as a chunk with `is_last == false`; the component reports `_OVERFLOW` and discards the
  frame instead of silently truncating it. Without this flag the driver truncates with nothing but
  a DRAM debug log.
- **Polarity is preserved**: `start_level` records the level of the first duration, so no
  receiver-specific inversion assumptions leak into playback.
- **De-duplication** treats a frame as a repeat when it arrives inside
  `CONFIG_IR_CAPTURE_DEDUP_WINDOW_MS` and either matches the previous frame within tolerance or is
  at most a third of its size (the classic short repeat code, e.g. NEC's 9 ms + 2.25 ms).

## Resource use

| Item | Value |
|---|---|
| RMT memory | 1 block (48 symbols) — no DMA |
| Static RAM | ~2 KB RX buffer + 2 × ~1 KB frames + 3 KB task stack |

## Verified IDF v6 API used

`rmt_new_rx_channel()`, `rmt_rx_register_event_callbacks()`, `rmt_receive()`, `rmt_enable()`,
`rmt_disable()`, `rmt_del_channel()`.
