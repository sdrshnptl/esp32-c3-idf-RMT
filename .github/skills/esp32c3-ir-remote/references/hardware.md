# Hardware, pins, RMT and LED reference

## Pin map

| Function | GPIO | Direction | Notes |
|---|---|---|---|
| IR receiver output | 4 | input | TSOP/VS1838-class demodulating receiver, idles **high**, active low |
| IR emitter drive | 5 | output | base/gate of NPN or N-MOSFET; IR LED + series resistor on 3V3/5V |
| Hotkey button | 0 | input | active-low to GND, internal pull-up; also `XTAL_32K_P` (unused on this board) |
| Status LED | 8 | output | onboard single blue LED, **active-low**, strapping pin |
| Console UART TX/RX | 20 / 21 | — | `esp_console` diagnostics |

Never repurpose: GPIO2 (strapping), GPIO8 (strapping + LED), GPIO9 (BOOT strapping),
GPIO18/19 (USB-JTAG/serial).

## RMT budget (ESP32-C3)

| Item | Value |
|---|---|
| Channels | 4 total (2 TX / 2 RX) |
| Memory | 48 words per channel, `SOC_RMT_SUPPORT_RX_PINGPONG=1` |
| Allocation | IR RX = 1 RX channel, IR TX = 1 TX channel, 2 channels spare |

## Verified IDF v6 API (`esp_driver_rmt`)

```c
// TX
rmt_tx_channel_config_t tx_cfg = {
    .gpio_num = BOARD_IR_TX_GPIO,
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = 1000000,          // 1 tick = 1 us
    .mem_block_symbols = 64,
    .trans_queue_depth = 4,
    .flags.invert_out = 0,
    .flags.with_dma = 0,
};
rmt_new_tx_channel(&tx_cfg, &tx_chan);
rmt_apply_carrier(tx_chan, &(rmt_carrier_config_t){
    .frequency_hz = 38000,
    .duty_cycle = 33.0f,               // CHECK UNIT in this IDF revision (header: 0~100%)
    .flags.polarity_active_low = 0,
});
rmt_new_copy_encoder(&(rmt_copy_encoder_config_t){0}, &enc);
rmt_enable(tx_chan);
rmt_transmit(tx_chan, enc, symbols, n_symbols * sizeof(rmt_symbol_word_t), &(rmt_transmit_config_t){.loop_count = 1});
rmt_tx_wait_all_done(tx_chan, timeout_ms);

// RX
rmt_rx_channel_config_t rx_cfg = {
    .gpio_num = BOARD_IR_RX_GPIO,
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = 1000000,
    .mem_block_symbols = 64,
    .flags.invert_in = 0,
};
rmt_new_rx_channel(&rx_cfg, &rx_chan);
rmt_rx_register_event_callbacks(rx_chan, &cbs, ctx);
rmt_enable(rx_chan);
rmt_receive(rx_chan, buf, sizeof(buf), &(rmt_receive_config_t){
    .signal_range_min_ns = 1000,
    .signal_range_max_ns = 50000000,
});
```

Callback data: `rmt_rx_done_event_data_t { received_symbols, num_symbols, flags.is_last }`.
The callback runs in ISR context — copy data out and notify a task; never do work there.

## Recording format

```
frame {
  uint8_t  start_level;     // level of the first mark
  uint16_t edge_count;      // number of durations
  uint16_t durations[];     // microseconds, alternating mark/space
}
```
A typical NEC frame is ~68 edges (~136 B); worst-case AC frames ~300 edges (~600 B).
Default `CONFIG_IR_MAX_EDGES = 512`.

## LED indication (single blue LED, active-low, LEDC PWM @ 5 kHz, 10-bit)

| State | Pattern |
|---|---|
| Boot | 3 x 120 ms flashes |
| BLE advertising | slow breathing, 2 s period, ~15 % |
| BLE connected | solid, 25 % brightness |
| Learn armed | 5 Hz blink |
| Capture OK | one 250 ms flash |
| IR playing | 30 ms flash per frame |
| Hotkey sequence | 2 x 80 ms pulse per step |
| Error | 8 Hz blink for 5 s |
| Factory reset armed | continuous fast strobe |

## Wiring cautions

- Drive the IR LED via NPN/MOSFET with a base/gate resistor; add bulk capacitance near the LED.
- Keep the LED circuit as designed on GPIO8; do not add external pulldowns (strapping).
- Cheap remotes are 36–40 kHz; keep carrier frequency per-command, default 38 kHz.
- Keep UART logging at WARN inside IR paths — blocking UART writes distort timing.
