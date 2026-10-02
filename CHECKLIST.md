# Project Checklist — ESP32-C3 IR Remote Controller

Single source of truth for progress. **Update the affected milestone in the same turn it is
completed — never batch updates at the end.**

Legend: `[ ]` not started · `[~]` in progress · `[x]` done

| # | Milestone | Status | Completed |
|---|---|---|---|
| M0 | Workspace tooling: Copilot skill, repo memory, checklist | `[x]` | 2026-10-02 |
| M1 | Build hygiene: sdkconfig.defaults, partitions, minimal build | `[x]` | 2026-10-02 |
| M2 | `board` + `led_indicator` (LEDC patterns) | `[x]` | 2026-10-02 |
| M3 | `ir_capture` (RMT RX, raw learn + repeat de-dup) | `[ ]` | — |
| M4 | `ir_playback` (RMT TX + 38 kHz carrier, repeats) | `[ ]` | — |
| M5 | `ir_store` (NVS metadata + SPIFFS blobs, export/import) | `[ ]` | — |
| M6 | `ble_transport` + `protocol` (NimBLE GATT, cJSON RPC, framing) | `[ ]` | — |
| M7 | `docs/` GitHub Pages Web Bluetooth dashboard | `[ ]` | — |
| M8 | `hotkey` (GPIO0 short press → ≤8 command sequence) | `[ ]` | — |
| M9 | Hardening: WDT, error paths, unit tests, review checklist | `[ ]` | — |

---

## M0 — Workspace tooling

- [x] Workspace agent instructions: `.github/copilot-instructions.md`
- [x] Project skill: `.github/skills/esp32c3-ir-remote/SKILL.md` (+ `references/hardware.md`, `references/protocol.md`)
- [x] Repository memory: `/memories/repo/esp32-c3-idf-rmt.md`
- [x] Checklist document: `CHECKLIST.md` (this file)

## M1 — Build hygiene

- [x] `sdkconfig.defaults` created (target, flash, custom partitions, NimBLE on, `-Os`)
- [x] `partitions.csv` = active 4 MB OTA-ready layout; `partitions_2mb.csv` = documented fallback
- [x] Top-level `CMakeLists.txt`: `idf_build_set_property(MINIMAL_BUILD ON)`
- [x] `main/main.c` boot skeleton; `main/CMakeLists.txt` REQUIRES = `bt`, `esp_driver_rmt`,
      `esp_driver_gpio`, `esp_driver_ledc`, `nvs_flash`, `spiffs`, `esp_timer`
- [x] `.vscode/settings.json`: clangd `--compile-commands-dir` fixed (was pointing at another project)
- [x] `idf.py build` succeeds with the custom partition table (image 131 KB)
- [x] Wi-Fi/lwIP/esp_netif **not linked**: no `esp_wifi_*` / `lwip_*` / `esp_netif_*` symbols in
      `build/esp32-c3-idf-RMT.map`
- [x] `esptool flash_id` → **4 MB (XMC), chip rev v0.4**; active layout: `otadata` @ `0xf000`,
      `ota_0` @ `0x20000` (1.8125 MB), `ota_1` @ `0x1F0000`, `storage` @ `0x3C0000` (256 KB)

> **Finding:** `ESP_WIFI_ENABLED` has no Kconfig prompt, so it cannot be disabled from
> `sdkconfig.defaults`. `esp_wifi`/`lwip`/`wpa_supplicant`/`esp_netif` enter the build graph
> only as transitive dependencies of `esp_phy`, and the linker removes all of their code because
> nothing references it. This is verified, not assumed.
>
> **Finding:** with `MINIMAL_BUILD ON`, Kconfig menus of pruned components do not exist — BT and
> RMT options are only visible once `main` REQUIRES those components.

## M2 — board + led_indicator

- [x] `components/board` (pin map from Kconfig, `board.h` public API, validation + reserved-pin warnings)
- [x] `components/led_indicator` (LEDC PWM backend, active-low via `output_invert`, static pattern tables)
- [x] States: boot, advertising, connected, learn, error, factory reset + capture-ok / IR-frame / hotkey pulses
- [x] Builds clean (145 KB image) and runs on hardware: boot log shows `pins: ir_rx=4 ir_tx=5 hotkey=0 led=8`
      and `LED indication ready (gpio 8, active-low, 5000 Hz, 10-bit)` with no runtime errors
- [x] Fixed on hardware: `ledc_set_duty_and_update()` requires the LEDC fade service → switched to
      `ledc_set_duty()` + `ledc_update_duty()`
- [ ] **User action:** visually confirm on the board — 3 quick flashes at boot, then slow breathing

## M3 — ir_capture

- [ ] RMT RX channel at 1 MHz, glitch filter + end-of-frame gap
- [ ] `rmt_symbol_word_t` → `uint16_t durations[]` + start level
- [ ] Repeat-frame de-duplication within tolerance
- [ ] Overflow → `E_FRAME_TOO_LARGE` (never silent truncation)

## M4 — ir_playback

- [ ] RMT TX channel + `rmt_apply_carrier(38 kHz, ~33 %)`
- [ ] Copy encoder path, >32767 µs gap splitting
- [ ] Repeats with configurable inter-frame gap
- [ ] Hardware verification (camera / logic analyzer)

## M5 — ir_store

- [ ] NVS: schema version, profiles, button metadata, hotkey sequences
- [ ] SPIFFS: raw IR blobs, one file per command
- [ ] `data.export` / `data.import` round-trip

## M6 — ble_transport + protocol

- [ ] `idf_component.yml` with `espressif/cjson`
- [ ] NimBLE GATT server: CMD/RSP/RAW/STATUS, MTU 512, advertising + scan response
- [ ] 4-byte chunk framing, reassembly, bounded JSON parse
- [ ] JSON-RPC dispatcher + event notifier
- [ ] Verified with nRF Connect and Chrome

## M7 — docs/ dashboard

- [ ] `docs/index.html`, `docs/app.js`, `docs/style.css`, `docs/manifest.webmanifest`, `docs/.nojekyll`
- [ ] Connect (Web Bluetooth), profile CRUD, learn flow, test play
- [ ] Hotkey editor (≤8 slots, per-slot delay + repeats)
- [ ] Import/export, event log, browser-support note

## M8 — hotkey

- [ ] GPIO0 ISR → queue only; `esp_timer` debounce
- [ ] Sequence engine (≤8 steps), LED + BLE progress events
- [ ] Short press only; presses ignored while playing

## M9 — Hardening

- [ ] Task WDT wired, `esp_err_t` audit across all components
- [ ] `esp_console` diagnostics, factory reset
- [ ] Unity `test_apps` for `ir_store` and framing
- [ ] Review against `esp-idf-industrial-firmware-standards` review checklist
