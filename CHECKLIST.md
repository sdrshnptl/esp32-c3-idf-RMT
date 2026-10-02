# Project Checklist — ESP32-C3 IR Remote Controller

Single source of truth for progress. **Update the affected milestone in the same turn it is
completed — never batch updates at the end.**

Legend: `[ ]` not started · `[~]` in progress · `[x]` done

| # | Milestone | Status | Completed |
|---|---|---|---|
| M0 | Workspace tooling: Copilot skill, repo memory, checklist | `[x]` | 2026-10-02 |
| M1 | Build hygiene: sdkconfig.defaults, partitions, minimal build | `[x]` | 2026-10-02 |
| M2 | `board` + `led_indicator` (LEDC patterns) | `[x]` | 2026-10-02 |
| M3 | `ir_capture` (RMT RX, raw learn + repeat de-dup) | `[x]` | 2026-10-02 |
| M4 | `ir_playback` (RMT TX + 38 kHz carrier, repeats) | `[x]` | 2026-10-02 |
| M5 | `ir_store` (NVS metadata + SPIFFS blobs, export/import) | `[x]` | 2026-10-02 |
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

- [x] `components/ir_types` — shared `ir_frame_t` / `ir_tx_params_t` (header-only, no cycles)
- [x] RMT RX channel at 1 MHz, glitch filter (`IR_CAPTURE_GLITCH_FILTER_NS`) + end-of-frame idle gap
      (`IR_CAPTURE_FRAME_GAP_MS`)
- [x] `rmt_symbol_word_t` → `uint16_t durations[]` + `start_level`, trailing EOF entries trimmed
- [x] Repeat-frame de-duplication (`IR_CAPTURE_DEDUP_WINDOW_MS`): full-frame repeats matched within
      tolerance **and** short repeat codes (NEC-style) detected by edge-count ratio
- [x] Overflow reported as `IR_CAPTURE_EVENT_OVERFLOW` instead of silent truncation — achieved by
      enabling `en_partial_rx` (without it the driver truncates with only a DRAM debug log)
- [x] ISR does no work beyond recording `num_symbols`/`is_last` + task notification; conversion and
      callback run in task context so NVS/BLE/LED calls are safe
- [x] Builds clean (157 KB image, +6 KB `.bss` for the 2 KB RX bounce buffer, 2 × 1 KB frames and a
      3 KB static capture task stack)
- [x] Flashed and running on hardware: channel creates on GPIO4, learn window arms, no crash loop
- [x] Diagnostic added: the firmware logs the receiver pin's idle level at init, on arming and on
      window close (GPIO4 reads a stable `1`, the correct idle state of a demodulating receiver)
- [x] **Bug found and fixed via the M4 loopback:** calling `rmt_receive()` while a reception was
      already in flight returns `ESP_ERR_INVALID_STATE`, so a second `ir_capture_learn_start()`
      failed and `ESP_ERROR_CHECK` turned it into a ~1.9 s reboot loop. The component now tracks
      `armed` and `ir_capture_learn_cancel()` performs `rmt_disable()`+`rmt_enable()` to abort a
      pending reception and return the channel to a known state.
- [x] **Verified on hardware with a real remote (TSOP1738 on GPIO4):**
      `IR frame: 67 edges, start level 0, total 67592 us` with
      `head: 8950 4531 517 1713 519 1711`
      - `8950 / 4531` = NEC 9 ms / 4.5 ms leader, `517 / 1713` = NEC logic-one (560/1690)
      - 67 edges totalling ~67.6 ms = a full NEC frame, so durations, ordering and the
        end-of-frame gap are all correct
      - `start level 0` = the receiver pulls low during a burst, so polarity is captured correctly
- [x] **Repeat de-duplication verified:** holding the button produced
      `IR repeat: 3 edges (ignored)` — the NEC 9 ms + 2.25 ms repeat code was classified as a
      repeat instead of creating a duplicate button
- [ ] Optional: repeat the capture with 2–3 other remotes to widen protocol coverage

## M4 — ir_playback

- [x] `components/ir_playback` — RMT TX channel at 1 MHz, copy encoder, `mem_block_symbols` 48
- [x] `rmt_apply_carrier()` per frame with the carrier frequency and duty from `ir_tx_params_t`;
      applying `0.33f` (a 0..1 fraction, **not** a percentage — the header comment is wrong)
- [x] Polarity handled per frame in `build_symbols()`: a demodulating receiver reports a burst as a
      low level while the emitter is driven active-high, so the levels are flipped when
      `start_level == 0`
- [x] Levels longer than 32767 µs split into consecutive pieces (15-bit RMT duration field)
- [x] Software repeats with a configurable `repeat_gap_ms`
- [x] `ir_playback_stop()` aborts an in-flight transmission via `rmt_disable()`/`rmt_enable()`
- [x] Transmits cleanly on hardware (`rmt_transmit` + `rmt_tx_wait_all_done` return `ESP_OK`)
- [x] **Loopback self-test** in the bring-up path: attempts up to 8 times, trying both burst
      polarities before declaring failure
- [x] **Verified on hardware — `loopback PASS on attempt 1 (burst reported as a low level)`:**
      `IR frame: 17 edges, start level 0, total 25274 us`. That is the transmitted 18-edge test
      frame arriving back through the air, so the emitter, the receiver, the 38 kHz carrier,
      symbol building, polarity inversion and both RMT channels are all confirmed working.
      The 17-vs-18 edge difference is the receiver dropping the final space, which merges into the
      end-of-frame idle gap — a capture artefact, not a fault.
- [x] Emitter blink test confirms GPIO5 drives the IR LED and the LED blinks at the commanded rate
- [x] **Diagnostic bug found and fixed (would have misled the whole commissioning):** the loopback
      ran *before* `ir_capture_set_callback()`, so the callback was still `NULL` and the test
      reported "receiver saw nothing" on every attempt even though the receiver was picking the
      emitter up perfectly. Every earlier "loopback FAILED" was a **false negative**. Lesson: a
      failing self-test is not evidence of a hardware fault — validate the test itself.
- [x] **Hardware note (not blocking):** driving the emitter pin with a long *DC* pulse tripped the
      brownout detector (`E BOD: Brownout detector was triggered`), resetting the board. Modulated
      bursts at 33 % duty do not. This points to a marginal emitter supply: drive the IR LED
      through a transistor/MOSFET with a proper series resistor and local bulk capacitance.
      Also check whether the IR LED is being driven directly from the pin.
- [ ] Optional: confirm reach across a room (the loopback only proves near-field illumination)

## M5 — ir_store

- [x] NVS (`irstore`): schema version, id counters, profile table, per-profile button tables,
      hotkey sequence. A schema mismatch wipes the store, because the blobs mirror the in-memory
      structs
- [x] SPIFFS (`storage` partition, mounted at `/spiffs`): one file per learned frame
      (`c%08x.bin`), a validated 22-byte header plus the raw `uint16_t` durations
- [x] Recursive mutex around every entry point, so BLE / hotkey / capture callbacks can all use it
- [x] `ir_store_export()` / `ir_store_import()` through read/write callbacks — the bundle is
      field-by-field little-endian (not a struct dump) so it survives internal layout changes
- [x] Import wipes first and restores the id counters above every imported id
- [x] **Verified on hardware (round-trip self-test):** create profile + button + hotkey, export
      136 bytes, factory reset, import, then verify the profile, button, raw 12-edge frame bytes,
      playback parameters and the 2-step hotkey all came back intact:
      `store test: PASS - profile, button, 12-edge frame, playback parameters and a 2-step hotkey
      all round-tripped through export/import`
- [x] SPIFFS mounted and reporting `0/233681 bytes used` on a fresh `storage` partition
- [x] M4 loopback re-checked after this change and still passes (no regression)

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
