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
| M6 | `ble_link` + `protocol` (NimBLE GATT, cJSON RPC, framing) | `[x]` | 2026-10-03 |
| M7 | `docs/` GitHub Pages Web Bluetooth dashboard | `[x]` | 2026-10-03 |
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

## M6 — ble_link + protocol

> **Renamed:** this component is `ble_link`, not `ble_transport`. NimBLE already declares
> `ble_transport_init()` / `ble_transport_deinit()` for its own HCI transport layer in
> `nimble/transport.h`; reusing the prefix made the two headers collide at compile time.

- [x] `components/ble_link` — NimBLE peripheral, GATT server, advertising, MTU negotiation
- [x] GATT layout: primary service `a1e90000-…` with CMD `…0001`, RSP `…0002`, RAW `…0003`,
      STATUS `…0004`; the service UUID is advertised so Web Bluetooth can filter on it, and the
      device name goes in the scan response (a 128-bit UUID costs 18 of the 31 adv bytes)
- [x] 4-byte chunk framing both ways (`[u16 total][u16 offset]`), in-order reassembly, and
      MTU-sized outbound chunking (`mtu - 3 - 4`)
- [x] **The NimBLE host task is never blocked:** the GATT access callback only does bounded memcpy
      work and queues the message; a dedicated link task invokes the RX handler, so the protocol
      layer may block on NVS/SPIFFS/IR freely
- [x] Bounded static memory: 2 pool buffers + 1 reassembly buffer, oversized messages rejected,
      and a full pool drops the message with a warning instead of growing without limit
- [x] **Verified in firmware on hardware:** `host synced, preferred MTU 512`,
      `GATT handles: CMD=16 RSP=18 RAW=21 STATUS=24`,
      `advertising as "IR-RMT-0992" with service a1e90000-6c2b-4f1a-9d3e-b1c2d3e4f5a6`
- [x] Regression check: the M5 storage round-trip and the M4 IR loopback both still pass
- [x] Bonus evidence for M5: the store survived a full reflash
      (`1 profile(s), 1 command(s), 502 bytes in SPIFFS`), proving real NVS+SPIFFS persistence
- [x] **Bug found and fixed:** `ble_gatts_find_chr()` called straight after `ble_gatts_add_svcs()`
      finds nothing, because the attribute database only exists once the host syncs. Under
      `ESP_ERROR_CHECK` that became a reboot loop. Handles are now captured in the
      `gatts_register_cb` during registration.
- [x] `components/protocol/idf_component.yml` declaring `espressif/cjson` — resolved and fetched
      from the Component Registry into `managed_components/espressif__cjson` (cJSON is not in-tree
      in ESP-IDF v6)
- [x] `components/protocol` — cJSON RPC dispatcher, learn flow, event notifier
- [x] Command surface: `sys.info`/`sys.stats`/`sys.factory_reset`,
      `profile.list`/`create`/`rename`/`delete`, `button.list`/`learn`/`learn_cancel`/`rename`/
      `delete`/`waveform`/`play`, `ir.play`, `hotkey.get`/`set`/`clear`
- [x] Events: `link`, `button.learned`, `button.learn_failed`
- [x] Envelope `{"id","ok","data"}` / `{"id","ok":false,"err":{"code","msg"}}` with a full error
      code set; response larger than the limit is replaced by `E_TOO_LARGE` rather than truncated
- [x] Learn is asynchronous and single-shot: `button.learn` arms the receiver and returns at once,
      the frame arrives whenever the user presses the button. An oversized frame keeps the session
      armed so the user can retry, a timeout or a disconnect clears it, and the LED follows
- [x] **Verified in firmware on hardware:**
      `protocol: ready (schema 1, request <= 2048 bytes, response <= 4096 bytes)`
- [x] **Bug found and fixed:** the first protocol build failed with a cascade of
      "implicit declaration of `ble_link_notify` / `ble_link_mtu` / `ble_link_is_connected` …"
      because `protocol.c` included every dependency header except `ble_link.h`
- [x] `main` now REQUIRES `protocol` and calls `protocol_init()` right after `ble_link_init()`;
      the protocol link handler drives the LED, so boot settles into `LED_STATE_ADVERTISING`
- [x] All four `APP_BRINGUP_*` self-tests flipped to `default n` (they would otherwise register
      their own capture callback and fight the protocol layer for the receiver)
- [x] Stale-build guard applied after flashing: the image contains the protocol strings
      (`profile.create`, `hotkey.set`, `E_UNKNOWN_CMD`, `button.learned`) and zero `bring-up`
      strings, confirming the self-tests really are gone and not silently still compiled in
- [x] **No-Wi-Fi invariant re-verified precisely.** A naive `grep -c 'esp_wifi_|lwip_|esp_netif_'`
      on the map returns 48 hits and looks alarming, but they are component *names* in generated
      `esp_err_codes` section names. The real test: the "Archive member included" section extracts
      **nothing** from `libesp_wifi.a` / `libesp_netif.a` / lwip, and `nm` finds no
      `esp_wifi_init` / `esp_wifi_start` / `esp_netif_init` / `esp_netif_new`. The only four
      Wi-Fi-prefixed symbols present are `esp_phy`'s `esp_wifi_power_domain_on/off` (and their
      `_bt_` aliases at identical addresses), which the **Bluetooth** controller needs because
      Wi-Fi and BT share the PHY power domain on the C3. These are PHY power helpers, not the
      Wi-Fi stack.
- [x] **External verification DONE (phone):** Chrome discovered and connected to `IR-RMT-0992`,
      negotiated **MTU 498**, subscribed to RSP, and held sessions lasting **3.5 minutes**
      (104634 → 315194 ms) instead of the 10 s failure loop. The dashboard drove nine learn sessions
      and the store grew to `2 profile(s), 8 command(s), 4016 bytes in SPIFFS` — and survived a
      reflash. Zero `E` lines in the entire session.

## M7 — docs/ dashboard

> No build step, no bundler, no dependencies. GitHub Pages serves these files verbatim, and the
> page talks to the device directly over Web Bluetooth. This is what makes the "no Wi-Fi" constraint
> workable: the client is a static page, the transport is Bluetooth.

- [x] `docs/index.html`, `docs/app.js`, `docs/style.css`, `docs/manifest.webmanifest`,
      `docs/icon.svg`, `docs/.nojekyll`, `docs/README.md`
- [x] Web Bluetooth transport: `requestDevice()` filtered on the service UUID, CMD/RSP
      characteristics resolved by UUID, RSP notifications subscribed
- [x] Client-side framing that **matches the firmware exactly**: `[u16 total][u16 offset]`
      little-endian, payload sized to `mtu - 3 - 4`
- [x] **Chunk ordering is enforced, not hoped for.** The firmware reassembler restarts on an
      out-of-order chunk, so every outbound message goes through a single serialised promise chain
      and each write is awaited before the next is queued. Two overlapping requests would otherwise
      corrupt each other.
- [x] The MTU is not exposed by Web Bluetooth, so the client starts conservative (23) and adopts the
      real value from `sys.info` before sending anything large
- [x] Connect / disconnect, profile CRUD, button CRUD, learn flow, play, waveform preview,
      hotkey editor (≤8 slots, per-slot delay + repeats, reorder, test), device info + stats,
      factory reset, raw request box, event log with TX/RX/event/error colouring
- [x] Hotkey editor builds its command picker by walking every profile's buttons, because hotkey
      steps reference a `commandId`, not a `(profileId, buttonId)` pair
- [x] Waveform preview drawn on a canvas: levels alternate from the stored `startLevel`, scaled to
      total frame duration, and `truncated` frames draw a partial trace instead of being refused
- [x] Frames sourced from the **demodulating receiver idle high** convention are drawn inverted,
      matching what the hardware actually sees
- [x] Names are rendered with `textContent`, never `innerHTML`, so a hostile remote name stored on
      the device cannot inject markup into the page
- [x] **Verified in a browser:** page renders, all five tabs switch, the log shows
      `dashboard ready — press Connect`, and no JavaScript errors are raised
- [x] Browser-support fallback verified for real: the embedded browser reports
      `navigator.bluetooth === undefined`, and the page correctly shows the
      "Web Bluetooth is not available here" banner instead of failing silently
- [x] Validated: `node --check docs/app.js` passes, the manifest is valid JSON, `icon.svg` is
      valid XML
- [x] **Pitfall found and handled — the LAN address looks like it works but cannot.** Loading
      `http://10.145.20.81:8000` from the phone renders the page perfectly (the server binds all
      interfaces and the phone fetched every asset), yet **Connect can never work**: plain HTTP on a
      LAN address is not a *secure context*, and Chrome does not expose `navigator.bluetooth`
      outside one. The page now distinguishes this from "browser does not support Web Bluetooth"
      and prints the origin plus the three concrete fixes (`adb reverse` + localhost, the
      `unsafely-treat-insecure-origin-as-secure` flag, or GitHub Pages) instead of silently showing
      a dead Connect button. Both branches verified in a real browser.
- [x] **Bug found and fixed — the dashboard could never receive a single reply.**
      `BluetoothRemoteGATTCharacteristic.value` is a `DataView`, and `DataView` has no `subarray()`,
      so `onNotify` threw a `TypeError` on the first chunk of every response. The exception escaped
      into the devtools console (invisible to the app's own log), the promise never settled, and the
      10 s RPC timeout tore the link down. On hardware this looked exactly like a crash: five
      identical ~10 s connect → subscribe → disconnect cycles, with the device answering correctly
      every single time. Fixed by re-viewing the bytes as a `Uint8Array` via
      `new Uint8Array(view.buffer, view.byteOffset + N, view.byteLength - N)`, and `onNotify` now
      try/catches and fails pending requests immediately so a client-side throw can never again
      masquerade as an unresponsive device.
- [x] **Firmware follow-ups from the same investigation:** the disconnect path logged a misleading
      `E ble_link: no central connected` at ERROR level for what is a completely normal race (a reply
      produced just as the peer leaves) — now a `ESP_LOGD` with the caller handling the code. And the
      `link` event was **removed**: a central can only be told about a connection after it has
      connected, discovered the service and subscribed, so a connect-time event is undeliverable by
      construction and a disconnect-time one has no peer left. Sending them cost 5–6 wasted chunk
      writes per connection and ran cJSON on the **NimBLE host task's 4 KB stack**, which is no place
      for a recursive JSON printer. The dashboard tracks link state from its own GATT callbacks.
- [x] Confirmed by log analysis that the M6/M7 "crash" was **not** a fault: no `Guru Meditation`,
      no `Brownout`, no watchdog, no stack canary, and `reason 531` is HCI 0x13 =
      `BLE_ERR_REMOTE_USER_TERM_CONN` — the phone hanging up. The board also re-advertised correctly
      after every cycle, and the store kept `4 profile(s), 1 command(s)` across a reflash.
- [x] Added `<link rel="icon">` pointing at `icon.svg`, which also stops the `favicon.ico` 404
- [ ] **Deferred on purpose — import/export UI.** `ir_store_export()`/`import()` exist, but the
      protocol deliberately does not expose them yet: a full bundle can exceed both the 2 KiB
      request and 4 KiB response limits. Doing it properly means streaming the bundle over the
      reserved RAW characteristic with its own offset framing, not raising the caps.
- [x] **External verification DONE (phone):** live discovery, connection, profile creation and the
      full learn flow all work from Chrome on the phone over `adb reverse` + `http://localhost:8000`.
      Two profiles were created and eight buttons learned end-to-end through the dashboard UI.
- [ ] **Not yet evidenced:** pressing **Play** from the dashboard (IR transmit) has not been
      confirmed in a captured log. Everything up to and including learn is proven; playback still
      needs one deliberate test, ideally with the receiving appliance watching.
- [x] **Console caveat discovered the hard way.** On this SuperMini `/dev/ttyACM0` **is the C3's own
      native USB peripheral** (it enumerates as "USB JTAG/serial debug unit | Espressif"), not a
      bridge chip. So *any* reset — including a routine re-flash — kills the console too, and a real
      brownout or panic would look identical: the log stops and the port vanishes with
      "device reports readiness to read but returned no data". Never conclude "no crash" from a
      stopped console; read the **next boot's** reset reason. The reset-reason log added to
      `main.c` is the correct mitigation, and it reported `USB peripheral (11)` =
      `ESP_RST_USB` — a benign host-driven reset, not brownout (9), panic (4) or watchdog (5/6/7).
      Also: only **one** reader may hold the port. The VS Code ESP-IDF extension's monitor and a
      manual `idf.py monitor` cannot share it — two readers produce the "multiple access" error and
      garbled output.

## M8 — hotkey

- [ ] GPIO0 ISR → queue only; `esp_timer` debounce
- [ ] Sequence engine (≤8 steps), LED + BLE progress events
- [ ] Short press only; presses ignored while playing

## M9 — Hardening

- [ ] Task WDT wired, `esp_err_t` audit across all components
- [ ] `esp_console` diagnostics, factory reset
- [ ] Unity `test_apps` for `ir_store` and framing
- [ ] Review against `esp-idf-industrial-firmware-standards` review checklist
