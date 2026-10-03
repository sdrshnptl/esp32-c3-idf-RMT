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

1. `rmt_carrier_config_t.duty_cycle` is a **float fraction (0..1)**, not a percentage — the header
   comment claiming "0~100%" is wrong. Use `0.33f` for a 33 % carrier.
2. `rmt_new_copy_encoder` requires the payload to already be `rmt_symbol_word_t[]` — not `uint16_t`.
3. RMT duration fields are 15-bit → any level longer than 32767 µs at 1 MHz resolution must be split,
   and `signal_range_max_ns` cannot exceed 32.767 ms at 1 MHz.
4. **RX overflow is silent by default.** The user buffer passed to `rmt_receive()` is the real limit;
   without `flags.en_partial_rx` the driver truncates with only a DRAM debug log. Enable it to
   receive an `is_last == false` chunk as an explicit overflow signal.
5. **Never call `rmt_receive()` while a reception is already in flight** — it fails with
   `ESP_ERR_INVALID_STATE` (the channel FSM is `RUN`, not `ENABLE`). Track an `armed` flag, and use
   `rmt_disable()` + `rmt_enable()` to abort a pending reception and return the channel to a usable
   state. Verified the hard way: it caused a ~1.9 s reboot loop via `ESP_ERROR_CHECK`.
6. **`ir_frame_t.durations[]` is `uint16_t` microseconds**, so one stored edge cannot exceed
   65535 µs. Assigning a larger value fails the build with `-Werror=overflow` (a 200000 µs value
   silently becomes 3392). Longer levels must be split by `ir_playback`'s symbol builder, and the
   frame itself can only carry the smaller value.
7. **A DC pulse on the emitter pin can brown out the board.** Driving GPIO5 continuously tripped
   the brownout detector (`E BOD: Brownout detector was triggered`), resetting the chip in a loop.
   Keep test transmissions carrier-modulated (a 33 % duty 38 kHz burst draws ~1/3 the average
   current), and treat any brownout on transmit as an emitter-drive hardware fault.
8. **The RMT RX drops the final space of a frame** — after the last mark the line stays idle, so
   that space merges into the end-of-frame gap. A transmitted N-edge frame comes back as N-1 edges.
   Do not assert exact edge equality when comparing a transmitted frame with a received one.
9. **Do not trust a failing self-test before validating the test itself.** The IR loopback reported
   "receiver saw nothing" for many cycles purely because it ran before `ir_capture_set_callback()`,
   leaving the callback `NULL`. Check the plumbing (callbacks registered, buffers wired) before
   concluding the hardware is at fault.
10. **`ESP_GOTO_ON_ERROR` / `ESP_GOTO_ON_FALSE` assign to a local variable named `ret`**, not to
    whatever error variable you happen to have. A function using them must declare `esp_err_t ret;`
    or the file will not compile.
11. **A Kconfig `string` interpolated with `%s` into a fixed buffer trips
    `-Werror=format-truncation`**, because the compiler assumes up to 255 bytes. Bound it with a
    precision (`"%.32s"`) or the build fails.
12. **NimBLE owns the `ble_transport_*` symbol prefix** (`ble_transport_init`, `ble_transport_deinit`
    in `nimble/transport.h`). A component exposing those names cannot include the NimBLE headers —
    the declarations conflict. Use a different prefix (`ble_link_*`).
13. **`ble_gatts_add_svcs()` does not create the attribute database.** `ble_gatts_find_chr()`
    returns nothing until the host has synced, so characteristic handles must be captured in
    `ble_hs_cfg.gatts_register_cb` (op `BLE_GATT_REGISTER_OP_CHR`, `ctxt->chr.val_handle`).
    Calling find_chr from init and wrapping it in `ESP_ERROR_CHECK` produces a reboot loop.
14. **The "no Wi-Fi" invariant cannot be checked with a plain `grep` on the map file** — it gives
    wrong answers in both directions. `grep -c 'esp_wifi_\|lwip_\|esp_netif_' build/*.map` returns
    ~48 hits that are *not* failures: they are component names inside generated `esp_err_codes`
    section names, plus the map's archive-list preamble. The real test is (a) the
    "Archive member included to satisfy reference" section must extract **nothing** from
    `libesp_wifi.a`, `libesp_netif.a` or lwip, and (b)
    `nm elf | grep -E 'esp_wifi_init|esp_wifi_start|esp_netif_init|esp_netif_new'` must be empty.
    With BLE enabled you should still **expect four** Wi-Fi-prefixed symbols:
    `esp_wifi_power_domain_on/off` plus their `esp_wifi_bt_power_domain_on/off` aliases at identical
    addresses. They come from `esp_phy` and are required by the **Bluetooth** controller because
    Wi-Fi and BT share the PHY power domain on the C3. They are PHY power-domain helpers, not the
    Wi-Fi stack.
15. **Verify the flashed artifact actually contains the change.** A stale object file has already
    shipped once in this project (object timestamp older than the source). After flashing, check
    `strings build/*.elf | grep -c '<new symbol>'` returns non-zero — and check a symbol that should
    have *disappeared* now returns zero.
16. **Bring-up self-tests that register a capture callback must be disabled** before the protocol
    layer takes over, or two callbacks fight over the same receiver. They are `default n`; leaving
    them `y` is a silent behaviour change, not a harmless extra log.
17. **Omitting one dependency header from a new component produces a cascade of
    "implicit declaration of `<dep>_...`" errors.** Read the *function* names in the errors — they
    name the missing header directly.
18. **`BluetoothRemoteGATTCharacteristic.value` is a `DataView`, not a `Uint8Array`.** `DataView`
    has no `subarray()` — that is a TypedArray method. `view.subarray(4)` throws a `TypeError`
    *inside* the `characteristicvaluechanged` handler, where it is invisible to the app's own logging
    and escapes to the devtools console. The awaiting promise never settles, so the request dies on
    its timeout and the client tears the link down. The hardware symptom is a device that answers
    every request correctly while the browser disconnects on a fixed schedule — five identical ~10 s
    connect/subscribe/disconnect cycles here, with `reason 531` (= HCI 0x13,
    `BLE_ERR_REMOTE_USER_TERM_CONN`, i.e. the phone hanging up normally). Use
    `new Uint8Array(view.buffer, view.byteOffset + N, view.byteLength - N)` instead.
19. **Always try/catch inside a BLE notification handler** and reject any in-flight requests
    immediately. An exception thrown there never reaches the app's own log, so a client-side bug
    presents exactly like an unresponsive device.
20. **`reason 531` is not a fault.** Subtracting NimBLE's HCI error base (512) gives 19 = 0x13 =
    `BLE_ERR_REMOTE_USER_TERM_CONN`: the central disconnected deliberately. Real faults announce
    themselves — `Guru Meditation Error`, `Brownout detector was triggered`,
    `Task watchdog got triggered`, `Stack canary watchpoint triggered`. Check for those before
    suspecting the firmware. Related: `ESP_LOGD` strings are removed at compile time when the log
    level is higher, so `strings elf | grep '<new LOGD text>'` returning 0 does *not* mean a stale
    build.
21. **The console IS the chip's own USB peripheral, so every reset hides its own evidence.**
    `/dev/ttyACM0` on this SuperMini is not a USB-UART bridge — the device enumerates as
    "USB JTAG/serial debug unit | Espressif". Consequently *any* reset (flash, brownout, panic,
    watchdog) also drops the console: the log simply stops, the port disappears, and `idf_monitor`
    reports "device reports readiness to read but returned no data (device disconnected or multiple
    access on port?)". A genuine brownout is therefore **indistinguishable from a routine re-flash**
    from the console alone. Mitigation: log `esp_reset_reason()` early in `app_main` and read the
    **next** boot. Index map from `esp_system.h`: 4 PANIC, 5 INT_WDT, 6 TASK_WDT, 7 WDT,
    **9 BROWNOUT** (red flag on this board), 11 USB (benign, host-driven). Also, only **one** reader
    may hold the port — the VS Code ESP-IDF extension's monitor and a manual `idf.py monitor` cannot
    share it, and two readers corrupt the output.
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
