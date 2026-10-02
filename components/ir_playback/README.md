# ir_playback

Replays a captured `ir_frame_t` through the IR emitter with a modulated carrier.

## API

```c
#include "ir_playback.h"

ESP_ERROR_CHECK(ir_playback_init());

ir_tx_params_t params = IR_TX_PARAMS_DEFAULT();
params.repeats = 2;
ESP_ERROR_CHECK(ir_playback_send(&frame, &params));   /* blocking */
```

| Function | Purpose |
|---|---|
| `ir_playback_init()` | Create the RMT TX channel and the copy encoder |
| `ir_playback_send()` | Transmit a frame (blocking, including repeats) |
| `ir_playback_stop()` | Abort the in-flight transmission |
| `ir_playback_is_busy()` | Whether a transmission is running |
| `ir_playback_deinit()` | Release the encoder and channel |

## Polarity

A demodulating receiver reports a carrier burst as a **low** level, but the IR emitter is driven
**active-high** through its transistor. `build_symbols()` therefore flips every stored level when
`frame->start_level == 0`. This keeps the captured data untouched and puts the polarity fix in one
place.

## Timing

- 1 MHz resolution → 1 tick = 1 µs.
- The RMT duration field is 15 bits, so a level longer than **32767 µs** is split into consecutive
  pieces. Capture can never produce one (the end-of-frame gap is capped at 32 ms), but imported
  frames can.
- Repeats are driven in software with a configurable `repeat_gap_ms`, which handles both plain
  frame repeats and protocols that need a distinct repeat code.

## Caller contract

`ir_playback_send()` **blocks** until the frame (plus repeats) has left the wire — up to a second or
more for repeated frames. Call it from a dedicated worker task, never from an ISR or from a
callback that must return quickly. `ir_playback_stop()` is not synchronised against a concurrent
send.

## Resource use

| Item | Value |
|---|---|
| RMT memory | 1 block (48 symbols) |
| Static RAM | 4 KB symbol buffer (`IR_FRAME_MAX_EDGES` × 8 B) |
