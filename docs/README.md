# `docs/` — Web Bluetooth dashboard

A plain static app: no build step, no bundler, no dependencies, no server. GitHub Pages serves
these files verbatim, and the page talks to the ESP32-C3 directly over Web Bluetooth.

> **This is deliberately not Wi-Fi.** The firmware never starts an AP or a station, and there is no
> on-device HTTP server. The page is the client; Bluetooth is the only transport.

## Files

| File | Purpose |
|---|---|
| `index.html` | Layout and controls |
| `style.css` | Dark, mobile-first theme |
| `app.js` | BLE transport, JSON-RPC client, all UI logic |
| `manifest.webmanifest`, `icon.svg` | Installable-app metadata |
| `.nojekyll` | Stops Jekyll from processing the directory |

## Deploying to GitHub Pages

1. Push the repository.
2. **Settings → Pages → Build and deployment → Source: Deploy from a branch**,
   branch `main`, folder **`/docs`**.
3. Open `https://<user>.github.io/<repo>/` **in Chrome on Android**.

`https://` on github.io is a secure context, which Web Bluetooth requires.

## Testing locally without deploying

`localhost` is also a secure context, so the phone can load the page straight off this PC:

```sh
# 1. serve docs/ (any static server works)
python3 -m http.server 8000 --directory docs

# 2. forward the phone's localhost to this PC (phone connected over USB with adb)
adb reverse tcp:8000 tcp:8000
```

Then open **`http://localhost:8000`** in Chrome on the phone. Nothing is exposed to the network.

### ⚠ Do not use this PC's LAN address

It is tempting to load `http://10.x.x.x:8000` from the phone instead. **The page will render, but
Connect will never work.** Plain HTTP on a LAN address is *not* a secure context, and Chrome does not
expose `navigator.bluetooth` outside one. The app now detects this and says so explicitly rather than
failing silently, but the fixes are:

| Fix | Notes |
|---|---|
| `adb reverse` + `http://localhost:8000` | Best option. No certificate, nothing on the network. |
| `chrome://flags/#unsafely-treat-insecure-origin-as-secure` | Add the LAN origin and restart Chrome. Fine for a quick check, do not ship it. |
| Deploy to GitHub Pages | The real target, and HTTPS by default. |

If you do want to serve on the LAN for browsing, bind all interfaces:

```sh
python3 -m http.server 8000 --directory docs --bind 0.0.0.0
```

## Browser requirements

- **Chrome on Android** — the only practical Web Bluetooth implementation here. Firefox on Android
  does not support it at all.
- Requires a **secure context** (HTTPS or `localhost`) and a **user gesture** — the Connect button.
- Android may need **Location** permission granted for Bluetooth scanning.
- The device must advertise the service UUID, because `requestDevice()` filters on it.

## Wire protocol

Two characteristics under service `a1e90000-6c2b-4f1a-9d3e-b1c2d3e4f5a6`:

| Characteristic | UUID | Used for |
|---|---|---|
| CMD | `…-0001` | requests, `write` / `writeWithoutResponse` |
| RSP | `…-0002` | responses **and events**, `notify` |
| RAW | `…-0003` | reserved for bulk frame data |
| STATUS | `…-0004` | short device info, `read` / `notify` |

Every chunk is framed as **`[u16 total][u16 offset]` little-endian** followed by payload, sized to
`mtu - 3 - 4`. Chunks must arrive **in order** — the firmware's reassembler restarts on an
out-of-order chunk — so `app.js` serialises all outbound messages through a single promise chain and
awaits each write.

```json
// → request
{"id":17,"cmd":"profile.create","args":{"name":"Living room"}}

// ← success
{"id":17,"ok":true,"data":{"id":1,"name":"Living room"}}

// ← failure
{"id":17,"ok":false,"err":{"code":"E_NO_PROFILE","msg":"no such profile"}}

// ← unsolicited event
{"evt":"button.learned","data":{"profileId":1,"buttonId":1,"commandId":1,"edges":67}}
```

The full command and event reference lives in `components/protocol/README.md`.

### Learning is asynchronous

`button.learn` answers immediately with `{learning:true}`; the frame arrives whenever the user
presses the remote button. The result comes back as `button.learned` or `button.learn_failed`, so
the UI arms, shows an "Listening…" hint, and waits. A single session captures a single press — arm
it again for the next button.

## Notes

- The MTU is not exposed by Web Bluetooth, so the client starts conservative (23) and takes the real
  value from `sys.info` before sending anything large.
- All names are rendered with `textContent`, never `innerHTML`, so a hostile remote name stored on
  the device cannot inject markup into this page.
- The waveform preview alternates levels starting from the stored `startLevel`, scaled to the total
  frame duration. `button.waveform` caps the array and sets `truncated`, so long frames draw a
  partial trace rather than being refused.
