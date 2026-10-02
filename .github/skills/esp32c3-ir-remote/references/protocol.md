# BLE protocol, JSON-RPC and storage reference

## Transport

| Item | Value |
|---|---|
| Stack | NimBLE, peripheral only, `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU = 512` |
| Advertising | 128-bit service UUID in the ad payload; device name `IR-RMT-XXXX` in the scan response |
| Security | Open GATT for MVP; bonding (Just Works) behind a Kconfig option |

## GATT characteristics

Base UUID: `a1e9XXXX-6c2b-4f1a-9d3e-b1c2d3e4f5a6`

| Char | Suffix | Properties | Purpose |
|---|---|---|---|
| CMD | `0001` | Write, Write-No-Rsp | JSON-RPC requests (chunked) |
| RSP | `0002` | Notify | JSON-RPC responses + events |
| RAW | `0003` | Write-No-Rsp / Notify | binary IR payload transport |
| STATUS | `0004` | Read, Notify | device info, MTU, heap, version, LED state |

All characteristics must be **primary** services for Web Bluetooth.

## Framing

Large payloads exceed the negotiated MTU, so every write is chunked with a 4-byte
little-endian header:

```
[uint16 total_len][uint16 offset][payload...]
```

JSON is only used for control/metadata. Raw IR durations travel on the RAW characteristic as
`uint16` LE values (no base64 bloat).

## JSON-RPC

Request:  `{"id":17,"cmd":"profile.get","args":{"id":3}}`
Response: `{"id":17,"ok":true,"data":{...}}`
Error:    `{"id":17,"ok":false,"err":{"code":"E_NO_SPACE","msg":"..."}}`
Event:    `{"evt":"ir.captured","data":{...}}`

| Group | Commands |
|---|---|
| System | `sys.info`, `sys.stats`, `sys.led_test`, `sys.reset`, `sys.factory_reset` |
| Profiles | `profile.list`, `profile.get`, `profile.create`, `profile.rename`, `profile.delete`, `profile.reorder` |
| Buttons | `button.learn_start`, `button.learn_cancel`, `button.save`, `button.rename`, `button.move`, `button.delete`, `button.test_play` |
| IR | `ir.play`, `ir.stop`, `ir.status` |
| Hotkey | `hotkey.get`, `hotkey.set`, `hotkey.test`, `hotkey.clear` |
| Data | `data.export`, `data.import`, `storage.stats` |
| Events | `ir.captured`, `ir.played`, `hotkey.fired`, `hotkey.step`, `led.state`, `log.msg` |

Bound the parser: `cJSON_ParseWithLengthOpts` with `CONFIG_PROTO_MAX_JSON` (default 2048) and
free the document immediately after dispatch.

## Storage

| Data | Location |
|---|---|
| Profile/button metadata, hotkey sequences, schema version | NVS |
| Raw IR blobs (durations + carrier params) | SPIFFS partition, one file per command |

NVS namespaces/keys are limited to 15-char names — keep keys short (`p<id>`, `c<id>`, `hk`).
Provide `data.export` / `data.import` so a whole configuration round-trips as one JSON document.

## Data model

```
profile { id, name, buttonCount, buttons[] { id, name, slot } }
command { id, profileId, buttonId, startLevel, edgeCount, durations[], repeats,
          repeatGapMs, carrierHz, dutyPct, repeatFrameId? }
hotkey  { profileId, steps[] { cmdId, delayMs, repeats } }   // steps.length <= 8
```
