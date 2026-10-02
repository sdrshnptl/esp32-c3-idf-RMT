---
name: esp32c3-ir-remote
description: >-
  Project-specific domain knowledge for the ESP32-C3 SuperMini universal infrared recorder/player.
  Use when: working in this workspace on IR capture or playback, RMT channels, NEC/raw IR timing,
  the BLE (Web Bluetooth) GATT protocol between the GitHub Pages dashboard and the device, profile
  or command persistence in NVS/SPIFFS, the GPIO0 hotkey sequence engine, LED status patterns,
  sdkconfig.defaults, or partition tables. Covers verified IDF v6 APIs, the locked pin map and
  data model, and the pitfall list for this board.
argument-hint: '[IR capture | IR playback | BLE protocol | storage | hotkey | LED]'
user-invocable: true
---

# ESP32-C3 SuperMini IR Remote — Project Skill

Universal IR remote: records arbitrary infrared frames protocol-agnostically over RMT RX, plays
them back over RMT TX with a 38 kHz carrier, stores profiles/commands on flash, and is driven by a
Web Bluetooth dashboard hosted on GitHub Pages. **No Wi-Fi, no on-device web server.**

## Locked decisions

| Topic | Decision |
|---|---|
| Board | ESP32-C3 SuperMini (RISC-V, 400 KB SRAM, flash size to be measured) |
| LED | **Single blue LED, GPIO8, active-low** → LEDC PWM + pattern language. There is no RGB LED. |
| IR RX / TX | GPIO4 / GPIO5 (Kconfig-overridable) |
| Hotkey | GPIO0, short press only; configuration is dashboard-only |
| Storage | NVS metadata + SPIFFS raw IR blobs |
| BLE | NimBLE, open GATT, bonding behind Kconfig |
| JSON | `espressif/cjson` from the Component Registry (cJSON is no longer in-tree) |
| Wi-Fi | Removed from the build entirely (`MINIMAL_BUILD ON`) |

## Reference files

| Topic | Reference |
|---|---|
| Hardware, pins, LED patterns, wiring cautions | [references/hardware.md](./references/hardware.md) |
| BLE GATT layout, JSON-RPC schema, framing, data model | [references/protocol.md](./references/protocol.md) |

## Core IR design

- **Capture** is protocol-agnostic: RMT RX at **1 MHz** (1 tick = 1 µs) with
  `signal_range_min_ns ≈ 1000` (glitch filter) and `signal_range_max_ns ≈ 50 ms` (end-of-frame).
  Flatten `rmt_symbol_word_t` pairs into `uint16_t durations[]` + a start level.
- **Playback** replays the recorded envelope with `rmt_apply_carrier({38 kHz, ~33 %})` and a copy
  encoder. Splitting gaps longer than 32767 µs into multiple symbols is required.
- De-duplicate repeat frames (repeats match the previous frame within tolerance) so learning a key
  does not create duplicate buttons.
- See `references/hardware.md` for the full API cheat sheet and pitfalls.

## Pitfall list (learned)

1. `rmt_carrier_config_t.duty_cycle` is a **float**; check its unit in the header of the IDF
   revision before use.
2. `rmt_new_copy_encoder` requires the payload to already be `rmt_symbol_word_t[]` — not `uint16_t`.
3. RMT duration fields are 15-bit → any level longer than 32767 µs at 1 MHz resolution must be split.
4. GPIO8 is a **strapping pin** and drives the LED — never repurpose it, never add a pulldown.
5. The IR LED must be driven through a transistor/MOSFET; a bare GPIO cannot source the burst current.
6. Web Bluetooth needs a **secure context** (GitHub Pages HTTPS or localhost) and a user gesture.
7. Web Bluetooth requires service UUIDs to be listed in `filters`/`optionalServices` and only
   exposes **primary** services.
8. A 128-bit service UUID plus flags nearly fills the 31-byte advertising payload → put the device
   name in the scan response.
9. Never assume `sdkconfig` edits persist — always use `sdkconfig.defaults`.
10. Progress is tracked in `CHECKLIST.md`; update it per milestone, never in bulk.
11. `IS_ENABLED()` is not universally available in ESP-IDF headers — use `#ifdef CONFIG_...` for
    Kconfig booleans (verified failure in this workspace).
12. `ledc_set_duty_and_update()` fails with *"Fade service not installed"* unless
    `ledc_fade_func_install()` was called. Use `ledc_set_duty()` + `ledc_update_duty()` instead
    (verified on hardware in this workspace).
13. With `MINIMAL_BUILD ON`, deleting `sdkconfig` is required for changed
    `sdkconfig.defaults` values to take effect — an existing `sdkconfig` wins over the defaults.
