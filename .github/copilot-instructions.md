# ESP32-C3 IR Remote Controller — Agent Instructions

Firmware for an **ESP32-C3 SuperMini** universal infrared recorder/player, controlled
exclusively over **BLE (Web Bluetooth)** from a static dashboard hosted on **GitHub Pages**.

## Hard constraints (never violate)

1. **No Wi-Fi, ever.** No AP, no STA, no on-device HTTP server, no mDNS. The dashboard is a
   BLE client served by GitHub Pages.
2. **Never edit `sdkconfig` by hand.** Use `sdkconfig.defaults` (and
   `sdkconfig.defaults.esp32c3` for target-specific options). `sdkconfig` is generated and git-ignored.
3. **Never embed HTML/CSS/JS as C string literals.** The web app lives in `docs/` and is deployed
   by GitHub Pages.
4. **Never modify ESP-IDF sources.** Extend through components only.
5. **Every `esp_err_t` must be checked** — `ESP_ERROR_CHECK`, `ESP_RETURN_ON_ERROR`, `ESP_GOTO_ON_ERROR`.
6. **Static allocation preferred.** No `malloc` in hot paths, ISRs, or the IR record/playback path.
7. **Respect the layering** `Application → Services → Drivers → HAL`. Only the `board` component
   may know GPIO numbers; every pin comes from `board.h` / Kconfig.
8. **`idf_build_set_property(MINIMAL_BUILD ON)` stays enabled** and `main`'s `REQUIRES` must be
   kept accurate — this is what keeps Wi-Fi out of the image.

## Verified environment

| Item | Value |
|---|---|
| ESP-IDF | **master** at `/home/sdrshnptl/.espressif/master/esp-idf` (v6.x-dev, git `fa8039b5`) |
| Toolchain | `riscv32-esp-elf-gcc` 16.1.0 |
| Target | `esp32c3` (min rev 3), single-core RISC-V |
| cJSON | **not in-tree** → use registry component `espressif/cjson` |
| RMT | component `esp_driver_rmt` (new `rmt_tx` / `rmt_rx` driver, not the legacy `driver/rmt.h`) |

## Pin map (locked)

| Function | GPIO | Notes |
|---|---|---|
| IR receiver output | **4** | demodulating receiver (TSOP/VS1838 class), idle high |
| IR emitter drive | **5** | via transistor/MOSFET — never drive the IR LED from the pin |
| Hotkey button | **0** | active-low to GND, internal pull-up, short press only |
| Status LED | **8** | onboard single blue LED, **active-low** (strapping pin) |
| Console UART | 20/21 | `esp_console` for bench debugging |

Avoid: GPIO2/8/9 (strapping), GPIO18/19 (USB-JTAG/serial).

## Verified IDF v6 RMT API (`esp_driver_rmt`)

```
TX : rmt_new_tx_channel(), rmt_transmit(), rmt_tx_wait_all_done(), rmt_tx_register_event_callbacks()
RX : rmt_new_rx_channel(), rmt_receive(), rmt_rx_register_event_callbacks()
Both: rmt_enable(), rmt_disable(), rmt_del_channel(), rmt_apply_carrier()
```

- `rmt_carrier_config_t { frequency_hz, duty_cycle (float), flags.polarity_active_low, flags.always_on }`
- `rmt_receive_config_t { signal_range_min_ns, signal_range_max_ns, flags.en_partial_rx }`
- `rmt_rx_done_event_data_t { received_symbols, num_symbols, flags.is_last }`
- `rmt_new_copy_encoder(&(rmt_copy_encoder_config_t){0}, &enc)` — payload must already be
  `rmt_symbol_word_t[]`. Use **1 MHz resolution** so 1 tick = 1 µs (max 32767 µs per level).
- Reference example: `examples/peripherals/rmt/ir_nec_transceiver`

## Build / flash

```
idf.py set-target esp32c3     # only once
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Milestone tracking (mandatory)

`CHECKLIST.md` at the repo root is the single source of truth for progress.
**Mark each milestone complete in the same turn it is finished — never batch updates.**

Project-specific domain knowledge is in the workspace skill
`.github/skills/esp32c3-ir-remote/` (IR/RMT details, BLE protocol, storage layout).
