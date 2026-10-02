# ir_types

Header-only component holding the data types shared by `ir_capture`, `ir_playback` and `ir_store`.

## Why a separate component

`ir_capture`, `ir_playback` and `ir_store` all need `ir_frame_t`. Putting it in any one of them
would force the others to depend on a sibling driver, which is the wrong layering. A header-only
component gives all three a common vocabulary with no dependency cycles.

## Types

```c
typedef struct {
    uint8_t  start_level;                        /* level of durations[0] */
    uint16_t edge_count;                         /* valid entries */
    uint16_t durations[IR_FRAME_MAX_EDGES];      /* microseconds */
} ir_frame_t;

typedef struct {
    uint32_t carrier_hz; uint8_t duty_percent;
    uint8_t repeats; uint16_t repeat_gap_ms;
} ir_tx_params_t;
```

`IR_FRAME_MAX_EDGES` comes from `CONFIG_IR_FRAME_MAX_EDGES` (menu: *IR frame types*). Note the
frame is ~1 KB when the default of 512 edges is used, so instances belong in static storage — never
on a task stack.

Storing `start_level` alongside the durations keeps playback polarity-correct without any
receiver-specific assumptions.
