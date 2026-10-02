# protocol

JSON-RPC over the BLE link. Translates dashboard requests into calls on `ir_store`, `ir_capture`,
`ir_playback` and `led_indicator`, and pushes asynchronous events back.

## Wire format

```json
// request
{"id":17,"cmd":"profile.create","args":{"name":"Living room"}}

// success
{"id":17,"ok":true,"data":{"id":1,"name":"Living room"}}

// failure
{"id":17,"ok":false,"err":{"code":"E_NO_PROFILE","msg":"no such profile"}}

// event (unsolicited)
{"evt":"button.learned","data":{"profileId":1,"buttonId":1,"commandId":1,"edges":67}}
```

`id` is echoed verbatim and may be any JSON value. Requests and responses are split into
MTU-sized chunks by `ble_link`.

## Commands

| Command | Args | Returns |
|---|---|---|
| `sys.info` | – | schema, app, idf, device, mtu, connected, uptimeMs, freeHeap, pins |
| `sys.stats` | – | profiles, commands, storageBytes, limits |
| `sys.factory_reset` | – | `{reset:true}` |
| `profile.list` | – | `profiles:[{id,name,buttons}]` |
| `profile.create` | `name` | `{id,name}` |
| `profile.rename` | `id,name` | `{}` |
| `profile.delete` | `id` | `{}` |
| `button.list` | `profileId` | `buttons:[{id,name,commandId}]` |
| `button.learn` | `profileId,name,timeoutMs?` | `{learning:true,timeoutMs}` — completes via event |
| `button.learn_cancel` | – | `{}` |
| `button.rename` | `profileId,buttonId,name` | `{}` |
| `button.delete` | `profileId,buttonId` | `{}` |
| `button.waveform` | `profileId,buttonId` | edges, startLevel, durations[], carrierHz, truncated |
| `button.play` | `profileId,buttonId,repeats?` | `{commandId,edges,repeats}` |
| `ir.play` | `commandId,repeats?` | `{commandId,edges}` |
| `hotkey.get` | – | `{configured,profileId,maxSteps,steps[]}` |
| `hotkey.set` | `profileId,steps[{commandId,delayMs,repeats}]` | `{steps:n}` |
| `hotkey.clear` | – | `{}` |

Error codes: `E_PARSE`, `E_INVALID`, `E_UNKNOWN_CMD`, `E_TOO_LARGE`, `E_NO_MEM`, `E_STORE`,
`E_NO_PROFILE`, `E_NO_BUTTON`, `E_NO_COMMAND`, `E_BUSY`, `E_CAPTURE`, `E_PLAYBACK`.

## Events

| Event | Data |
|---|---|
| `button.learned` | `{profileId, buttonId, commandId, name, edges, startLevel, durations[]}` |
| `button.learn_failed` | `{reason}` — `timeout`, `frame_too_large`, or a store error name |

There is deliberately **no `link` event**. A central can only be told about a connection after it has
connected, discovered the service and subscribed to RSP, so a connect-time event is undeliverable by
construction, and a disconnect-time one has no peer left to receive it. Clients track link state from
their own GATT callbacks.

## The learn flow

`button.learn` is **asynchronous on purpose**. It arms the receiver and answers immediately; the
frame arrives whenever the user presses the button, so blocking the request would stall every other
command for up to the timeout. The client waits for `button.learned` or `button.learn_failed`.

A session is **single shot**: after a frame is stored the receiver is disarmed and the client arms
again for the next button. An oversized frame keeps the session armed so the user can simply retry,
and a disconnect cancels any session in flight.

## Design notes

- **Bounded parsing.** Requests are copied into a fixed buffer, NUL-terminated, then parsed with
  `cJSON_ParseWithLength` against that known length — no reliance on a terminator inside the
  incoming buffer and no unbounded allocation.
- **Bounded responses.** A response larger than `CONFIG_PROTOCOL_MAX_RESPONSE` is replaced by an
  `E_TOO_LARGE` error rather than being sent truncated or in pieces the peer did not ask for.
  `button.waveform` caps its duration array and sets `truncated`.
- **No heap in the hot path beyond JSON.** Profile and button listing use static arrays.
  `ir_frame_t` instances are static because a frame is ~1 KB.
- **Blocking is allowed and safe.** This runs on the `ble_link` RX task and the `ir_capture` task,
  never the NimBLE host task, so NVS, SPIFFS and IR playback calls cannot stall the Bluetooth stack.
- **cJSON comes from the Component Registry** (`espressif/cjson`, see `idf_component.yml`) because
  it is no longer bundled with ESP-IDF.
