/*
 * IR Remote — Web Bluetooth dashboard for the ESP32-C3 IR recorder/player.
 *
 * Talks JSON-RPC to the firmware over two GATT characteristics:
 *   CMD (write / write-no-response)  requests,  chunked
 *   RSP (notify)                     responses and events, chunked
 *
 * Framing matches components/ble_link: every chunk is
 *   [u16 total][u16 offset] little-endian, then up to (mtu - 3 - 4) payload bytes.
 * Chunks must arrive in order — the firmware reassembler restarts on an out-of-order chunk —
 * so all outbound messages go through one serialised queue.
 *
 * Requests whose response arrives asynchronously (button.learn) resolve via events, not the RPC
 * reply, so they are deliberately fire-and-forget.
 */

'use strict';

/* ── protocol constants ─────────────────────────────────────────────────── */

const SERVICE_UUID = 'a1e90000-6c2b-4f1a-9d3e-b1c2d3e4f5a6';
const CHR = {
  cmd:    'a1e90001-6c2b-4f1a-9d3e-b1c2d3e4f5a6',
  rsp:    'a1e90002-6c2b-4f1a-9d3e-b1c2d3e4f5a6',
  raw:    'a1e90003-6c2b-4f1a-9d3e-b1c2d3e4f5a6',
  status: 'a1e90004-6c2b-4f1a-9d3e-b1c2d3e4f5a6',
};

const FRAME_HEADER_LEN = 4;
const ATT_NOTIFY_OVERHEAD = 3;   // opcode + handle
const DEFAULT_MTU = 23;          // until the MTU exchange completes
const RPC_TIMEOUT_MS = 10000;
const LEARN_TIMEOUT_MS = 20000;
const MAX_LOG_ENTRIES = 400;

const $ = (sel) => document.querySelector(sel);

/* ── state ──────────────────────────────────────────────────────────────── */

const state = {
  device: null,
  server: null,
  chars: { cmd: null, rsp: null },
  mtu: DEFAULT_MTU,
  nextId: 0,
  pending: new Map(),          // id -> {resolve, reject, timer}
  txQueue: Promise.resolve(),  // serialises outbound messages
  learn: null,                 // pending learn request context
  profiles: [],
  buttons: [],
  cursorProfile: 0,            // profile shown in the Buttons tab
  commands: [],                // flat command list for the hotkey editor
  hotkey: [],                  // local hotkey steps being edited
};

/* tiny event bus for firmware events */
const bus = new EventTarget();
const emit = (name, detail) => bus.dispatchEvent(new CustomEvent(name, { detail }));

/* ── utilities ──────────────────────────────────────────────────────────── */

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function el(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === 'class') node.className = v;
    else if (k === 'text') node.textContent = v;
    else if (k === 'html') node.innerHTML = v;
    else if (k.startsWith('on')) node.addEventListener(k.slice(2).toLowerCase(), v);
    else if (v !== null && v !== undefined) node.setAttribute(k, v);
  }
  for (const c of children.flat()) {
    if (c === null || c === undefined || c === false) continue;
    node.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return node;
}

function toast(msg, kind = '') {
  const box = $('#toasts');
  const node = el('div', { class: kind, text: msg });
  box.append(node);
  setTimeout(() => node.remove(), 4000);
}

/** Log one line in the Log tab. */
function logLine(kind, text) {
  const list = $('#log-list');
  const now = new Date();
  const stamp = now.toTimeString().slice(0, 8) + '.' + String(now.getMilliseconds()).padStart(3, '0');
  list.append(el('li', { class: kind }, el('span', { class: 't', text: stamp }), text));
  while (list.childElementCount > MAX_LOG_ENTRIES) list.firstElementChild.remove();
  if ($('#log-autoscroll').checked) list.lastElementChild.scrollIntoView({ block: 'nearest' });
}

/** Replace a host element's children. */
function render(host, ...nodes) {
  host.replaceChildren(...nodes.flat().filter(Boolean));
}

function bytesToHex(bytes) {
  return [...bytes].map((b) => b.toString(16).padStart(2, '0')).join(' ');
}

/* ── transport ──────────────────────────────────────────────────────────── */

function chunkPayloadSize() {
  // Conservative floor of 1 keeps a pathological tiny MTU from stalling the loop.
  return Math.max(1, state.mtu - ATT_NOTIFY_OVERHEAD - FRAME_HEADER_LEN);
}

/** Send one framed message, split into MTU-sized writes, in order. */
async function sendMessage(text) {
  if (!state.chars.cmd) throw new Error('not connected');

  const bytes = new TextEncoder().encode(text);
  const chunk = chunkPayloadSize();
  const useNoRsp = state.chars.cmd.properties.writeWithoutResponse === true;

  for (let offset = 0; offset < bytes.length; offset += chunk) {
    const n = Math.min(chunk, bytes.length - offset);
    const frame = new Uint8Array(FRAME_HEADER_LEN + n);
    frame[0] = bytes.length & 0xff;
    frame[1] = (bytes.length >> 8) & 0xff;
    frame[2] = offset & 0xff;
    frame[3] = (offset >> 8) & 0xff;
    frame.set(bytes.subarray(offset, offset + n), FRAME_HEADER_LEN);

    // Awaiting every write is what guarantees chunk ordering on the wire.
    if (useNoRsp) await state.chars.cmd.writeValueWithoutResponse(frame);
    else await state.chars.cmd.writeValueWithResponse(frame);
  }
}

/** Queue a message so two requests can never interleave their chunks. */
function enqueueSend(text) {
  const run = state.txQueue.then(() => sendMessage(text));
  // Keep the chain alive after a failure so later sends are not blocked forever.
  state.txQueue = run.catch(() => {});
  return run;
}

/* inbound reassembly */
const rx = { total: 0, got: 0, buf: null };

function onNotify(event) {
  try {
    handleChunk(event.target.value);
  } catch (err) {
    // Without this, an exception escapes into the devtools console, the caller never settles and
    // the request just times out - which looks exactly like a device that stopped responding.
    logLine('err', `chunk handling failed: ${err.message}`);
    failPending(new Error(`chunk handling failed: ${err.message}`));
  }
}

/** Reassemble one notification chunk and dispatch the message once it is complete. */
function handleChunk(view) {
  if (!view || view.byteLength < FRAME_HEADER_LEN) throw new Error('short chunk');

  const total = view.getUint16(0, true);
  const offset = view.getUint16(2, true);

  // `.value` is a DataView, which has no subarray() - that is a Uint8Array method. Re-view the
  // same bytes as a Uint8Array instead of calling a method that does not exist.
  const payload = new Uint8Array(view.buffer, view.byteOffset + FRAME_HEADER_LEN,
                                 view.byteLength - FRAME_HEADER_LEN);

  if (offset === 0) {
    rx.total = total;
    rx.got = 0;
    rx.buf = new Uint8Array(total);
  }
  if (rx.buf === null || total !== rx.total || offset !== rx.got) {
    rx.buf = null;
    throw new Error(`out-of-order chunk (offset ${offset}, expected ${rx.got})`);
  }

  rx.buf.set(payload, offset);
  rx.got += payload.length;
  if (rx.got < rx.total) return;

  const text = new TextDecoder().decode(rx.buf);
  rx.buf = null;
  rx.got = 0;

  let msg;
  try {
    msg = JSON.parse(text);
  } catch {
    throw new Error(`unparseable message: ${text}`);
  }
  handleMessage(msg);
}

function handleMessage(msg) {
  if (msg.evt !== undefined) {
    logLine('evt', `${msg.evt} ${JSON.stringify(msg.data ?? {})}`);
    emit(msg.evt, msg.data || {});
    return;
  }

  const entry = state.pending.get(msg.id);
  if (!entry) {
    logLine('rx', JSON.stringify(msg));
    return;
  }
  state.pending.delete(msg.id);
  clearTimeout(entry.timer);

  if (msg.ok) {
    logLine('rx', JSON.stringify(msg.data ?? {}));
    entry.resolve(msg.data ?? {});
  } else {
    const code = msg.err?.code || 'E_?';
    const text = msg.err?.msg || 'request failed';
    logLine('err', `${code}: ${text}`);
    const err = new Error(text);
    err.code = code;
    entry.reject(err);
  }
}

/** Reject every request still waiting for a reply. */
function failPending(err) {
  for (const [, entry] of state.pending) {
    clearTimeout(entry.timer);
    entry.reject(err);
  }
  state.pending.clear();
}

/**
 * JSON-RPC request.
 * @param {string} cmd      command name, e.g. "profile.list"
 * @param {object} [args]   arguments object, omitted when undefined
 * @param {number} [timeoutMs]
 */
function request(cmd, args, timeoutMs = RPC_TIMEOUT_MS) {
  if (!state.device?.gatt?.connected) return Promise.reject(new Error('not connected'));

  const id = ++state.nextId;
  const msg = { id, cmd };
  if (args !== undefined) msg.args = args;

  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      state.pending.delete(id);
      reject(new Error(`${cmd} timed out`));
    }, timeoutMs);

    state.pending.set(id, { resolve, reject, timer });
    logLine('tx', `${cmd} ${args === undefined ? '' : JSON.stringify(args)}`);
    enqueueSend(JSON.stringify(msg)).catch((err) => {
      state.pending.delete(id);
      clearTimeout(timer);
      reject(err);
    });
  });
}

/* ── connection ─────────────────────────────────────────────────────────── */

function setLink(connected, meta = '') {
  $('#link-dot').className = 'dot ' + (connected ? 'on' : 'off');
  $('#link-status').textContent = connected
    ? (state.device?.name || 'connected')
    : 'disconnected';
  $('#link-meta').textContent = meta;
  $('#btn-connect').textContent = connected ? 'Disconnect' : 'Connect';
  $('#btn-connect').classList.toggle('primary', !connected);
}

async function connect() {
  if (!navigator.bluetooth) {
    updateSupportBanner();
    return toast('Web Bluetooth is unavailable', 'err');
  }

  try {
    $('#link-dot').className = 'dot busy';
    $('#link-status').textContent = 'choosing…';

    state.device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [SERVICE_UUID] }],
    });
    state.device.addEventListener('gattserverdisconnected', onDisconnected);

    state.server = await state.device.gatt.connect();
    const service = await state.server.getPrimaryService(SERVICE_UUID);

    const chars = await service.getCharacteristics();
    state.chars.cmd = chars.find((c) => c.uuid === CHR.cmd) || null;
    state.chars.rsp = chars.find((c) => c.uuid === CHR.rsp) || null;
    if (!state.chars.cmd || !state.chars.rsp) throw new Error('CMD/RSP characteristics missing');

    state.chars.rsp.addEventListener('characteristicvaluechanged', onNotify);
    await state.chars.rsp.startNotifications();

    state.mtu = DEFAULT_MTU;              // refined by the first sys.info
    state.cursorProfile = 0;
    state.profiles = [];
    state.buttons = [];
    state.hotkey = [];

    setLink(true);
    toast('Connected', 'ok');

    const info = await request('sys.info');
    state.mtu = Math.min(info.mtu || DEFAULT_MTU, 512);
    setLink(true, `MTU ${state.mtu} · chunk ${chunkPayloadSize()} B`);
    logLine('evt', `device ${info.device} app ${info.app} mtu ${state.mtu}`);

    showTab('profiles');
    await refreshProfiles();
    await refreshDevice();
    await refreshHotkey();
  } catch (err) {
    if (err?.name === 'NotFoundError') {   // user dismissed the chooser
      setLink(false);
      return;
    }
    console.error(err);
    logLine('err', String(err.message || err));
    toast(String(err.message || err), 'err');
    await disconnect();
  }
}

async function disconnect() {
  try { await state.chars.rsp?.stopNotifications(); } catch { /* already gone */ }
  try { state.device?.gatt?.disconnect(); } catch { /* already gone */ }
  onDisconnected();
}

function onDisconnected() {
  failPending(new Error('connection lost'));
  state.chars = { cmd: null, rsp: null };
  state.server = null;
  state.mtu = DEFAULT_MTU;
  state.learn = null;
  state.profiles = [];
  state.buttons = [];
  state.hotkey = [];
  rx.buf = null;
  setLink(false);
  render($('#profile-list'), el('li', { class: 'empty', text: 'Connect to load profiles.' }));
  render($('#button-list'), el('li', { class: 'empty', text: 'No remote selected.' }));
  render($('#device-info'), el('dd', { class: 'empty', text: 'Connect to load device info.' }));
  render($('#hotkey-list'));
  render($('#profile-select'), el('option', { value: '', text: '—' }));
  setLearning(false);
}

/* ── profiles ───────────────────────────────────────────────────────────── */

async function refreshProfiles() {
  const data = await request('profile.list');
  state.profiles = data.profiles || [];
  renderProfiles();
  renderProfileSelect();
}

function renderProfiles() {
  const list = $('#profile-list');
  if (!state.profiles.length) {
    render(list, el('li', { class: 'empty', text: 'No remotes yet — add one above.' }));
    return;
  }

  render(list, state.profiles.map((p) => {
    const actions = [
      el('button', {
        class: 'icon', type: 'button', text: 'Buttons',
        onclick: () => { state.cursorProfile = p.id; showTab('buttons'); renderProfileSelect(); refreshButtons(); },
      }),
      el('button', {
        class: 'icon', type: 'button', text: 'Rename',
        onclick: async () => {
          const name = await askText('Rename remote', p.name);
          if (name === null) return;
          await run(() => request('profile.rename', { id: p.id, name }), 'Renamed');
          await refreshProfiles();
        },
      }),
      el('button', {
        class: 'icon danger', type: 'button', text: 'Delete',
        onclick: async () => {
          if (!confirm(`Delete "${p.name}" and its ${p.buttons} button(s)?`)) return;
          await run(() => request('profile.delete', { id: p.id }), 'Deleted');
          await refreshProfiles();
          await refreshButtons();
        },
      }),
    ];

    return el('li', { class: p.id === state.cursorProfile ? 'selected' : '' },
      el('div', { class: 'li-top' },
        el('span', { class: 'li-name', text: p.name }),
        el('span', { class: 'li-sub', text: `#${p.id} · ${p.buttons} button(s)` })),
      el('div', { class: 'li-actions' }, actions));
  }));
}

function renderProfileSelect() {
  const select = $('#profile-select');
  if (!state.profiles.length) {
    render(select, el('option', { value: '', text: '— none —' }));
    return;
  }
  if (!state.profiles.some((p) => p.id === state.cursorProfile)) {
    state.cursorProfile = state.profiles[0].id;
  }
  render(select, state.profiles.map((p) =>
    el('option', { value: p.id, text: p.name, selected: p.id === state.cursorProfile ? '' : null })));
}

/* ── buttons ────────────────────────────────────────────────────────────── */

async function refreshButtons() {
  const list = $('#button-list');
  if (!state.cursorProfile) {
    render(list, el('li', { class: 'empty', text: 'No remote selected.' }));
    state.buttons = [];
    return;
  }

  try {
    const data = await request('button.list', { profileId: state.cursorProfile });
    state.buttons = data.buttons || [];
  } catch (err) {
    render(list, el('li', { class: 'empty', text: `Could not load buttons: ${err.message}` }));
    return;
  }

  if (!state.buttons.length) {
    render(list, el('li', { class: 'empty', text: 'No buttons yet — name one and press Learn.' }));
    return;
  }

  render(list, state.buttons.map((b) => {
    const canvas = el('canvas', { class: 'wave hidden' });

    const actions = [
      el('button', { class: 'icon primary', type: 'button', text: 'Play', onclick: () => playButton(b) }),
      el('button', {
        class: 'icon', type: 'button', text: 'Waveform',
        onclick: async () => {
          const data = await run(() => request('button.waveform', {
            profileId: state.cursorProfile, buttonId: b.id,
          }));
          if (!data) return;
          canvas.classList.remove('hidden');
          drawWaveform(canvas, data);
          logLine('rx', `${b.name}: ${data.edges} edges, carrier ${data.carrierHz} Hz`
            + (data.truncated ? ' (preview truncated)' : ''));
        },
      }),
      el('button', {
        class: 'icon', type: 'button', text: 'Rename',
        onclick: async () => {
          const name = await askText('Rename button', b.name);
          if (name === null) return;
          await run(() => request('button.rename', {
            profileId: state.cursorProfile, buttonId: b.id, name,
          }), 'Renamed');
          await refreshButtons();
        },
      }),
      el('button', {
        class: 'icon danger', type: 'button', text: 'Delete',
        onclick: async () => {
          if (!confirm(`Delete button "${b.name}"?`)) return;
          await run(() => request('button.delete', {
            profileId: state.cursorProfile, buttonId: b.id,
          }), 'Deleted');
          await refreshButtons();
        },
      }),
    ];

    return el('li', {},
      el('div', { class: 'li-top' },
        el('span', { class: 'li-name', text: b.name }),
        el('span', { class: 'li-sub', text: `cmd #${b.commandId}` })),
      el('div', { class: 'li-actions' }, actions),
      canvas);
  }));
}

async function playButton(button) {
  if (!button.commandId) return toast('This button has no learned frame', 'err');
  const data = await run(() => request('button.play', {
    profileId: state.cursorProfile, buttonId: button.id,
  }));
  if (data) toast(`Sent ${button.name} (${data.edges} edges)`, 'ok');
}

/* ── learn ──────────────────────────────────────────────────────────────── */

function setLearning(active) {
  state.learn = active ? state.learn : null;
  $('#btn-learn').disabled = active;
  $('#btn-learn-cancel').disabled = !active;
  $('#learn-hint').classList.toggle('hidden', !active);
  if (active) {
    $('#learn-hint').textContent =
      'Listening — point the remote at the receiver and press the button once.';
  }
}

async function startLearn() {
  const name = $('#new-button').value.trim();
  if (!name) return toast('Give the button a name first', 'err');
  if (!state.cursorProfile) return toast('Select a remote first', 'err');

  state.learn = { profileId: state.cursorProfile, name };
  try {
    await request('button.learn', {
      profileId: state.cursorProfile, name, timeoutMs: LEARN_TIMEOUT_MS,
    });
    setLearning(true);
  } catch (err) {
    state.learn = null;
    if (err.code !== 'E_BUSY') toast(err.message, 'err');
    else setLearning(true);
  }
}

bus.addEventListener('button.learned', async (ev) => {
  const d = ev.detail;
  setLearning(false);
  $('#new-button').value = '';
  toast(`Learned "${d.name}" — ${d.edges} edges`, 'ok');
  state.cursorProfile = d.profileId || state.cursorProfile;
  await refreshButtons();
  await refreshProfiles();
  await refreshHotkey();
});

bus.addEventListener('button.learn_failed', (ev) => {
  setLearning(false);
  toast(`Learn failed: ${ev.detail.reason}`, 'err');
});

/* ── hotkey editor ──────────────────────────────────────────────────────── */

async function refreshHotkey() {
  const data = await request('hotkey.get');
  state.hotkey = (data.steps || []).map((s) => ({ ...s }));
  $('#hotkey-max').textContent = data.maxSteps ?? 8;
  await buildCommandPicker();
  renderHotkey();
}

/** Flat list of every button on the device, so the hotkey editor can reference command ids. */
async function buildCommandPicker() {
  state.commands = [];
  for (const p of state.profiles) {
    try {
      const data = await request('button.list', { profileId: p.id });
      for (const b of data.buttons || []) {
        if (b.commandId) state.commands.push({ ...b, profileId: p.id, profileName: p.name });
      }
    } catch { /* a profile that vanished mid-scan is not worth failing over */ }
  }

  const select = $('#hotkey-command');
  if (!state.commands.length) {
    render(select, el('option', { value: '', text: '— learn a button first —' }));
    return;
  }
  render(select, state.commands.map((c) =>
    el('option', { value: c.commandId, text: `${c.profileName} / ${c.name} (#${c.commandId})` })));
}

function commandLabel(commandId) {
  const c = state.commands.find((x) => x.commandId === commandId);
  return c ? `${c.profileName} / ${c.name}` : `command #${commandId}`;
}

function renderHotkey() {
  const host = $('#hotkey-list');
  if (!state.hotkey.length) {
    render(host, el('li', { class: 'empty', text: 'No steps yet. The hotkey does nothing.' }));
    return;
  }

  render(host, state.hotkey.map((step, i) => el('li', {},
    el('div', { class: 'li-top' },
      el('span', { class: 'li-name', text: `${i + 1}. ${commandLabel(step.commandId)}` }),
      el('span', {
        class: 'li-sub',
        text: `${step.delayMs} ms before · ${step.repeats} extra repeat(s)`,
      })),
    el('div', { class: 'li-actions' }, [
      el('button', {
        class: 'icon', type: 'button', text: '↑', disabled: i === 0 ? '' : null,
        onclick: () => {
          [state.hotkey[i - 1], state.hotkey[i]] = [state.hotkey[i], state.hotkey[i - 1]];
          renderHotkey();
        },
      }),
      el('button', {
        class: 'icon', type: 'button', text: '↓', disabled: i === state.hotkey.length - 1 ? '' : null,
        onclick: () => {
          [state.hotkey[i + 1], state.hotkey[i]] = [state.hotkey[i], state.hotkey[i + 1]];
          renderHotkey();
        },
      }),
      el('button', {
        class: 'icon danger', type: 'button', text: 'Remove',
        onclick: () => { state.hotkey.splice(i, 1); renderHotkey(); },
      }),
    ]))));
}

function addHotkeyStep() {
  const commandId = Number($('#hotkey-command').value);
  if (!commandId) return toast('Learn a button first', 'err');
  if (state.hotkey.length >= Number($('#hotkey-max').textContent || 8)) {
    return toast('Hotkey is full', 'err');
  }
  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, Number(v) || 0));
  state.hotkey.push({
    commandId,
    delayMs: clamp($('#hotkey-delay').value, 0, 60000),
    repeats: clamp($('#hotkey-repeats').value, 0, 10),
  });
  renderHotkey();
}

/** Replay the sequence from the browser, to check the order before committing it. */
async function testHotkey() {
  if (!state.hotkey.length) return toast('No steps to test', 'err');
  if (!state.cursorProfile) return toast('Select a remote first', 'err');

  try {
    for (const step of state.hotkey) {
      if (step.delayMs) await sleep(step.delayMs);
      await request('ir.play', { commandId: step.commandId, repeats: step.repeats });
    }
    toast('Test sequence sent', 'ok');
  } catch (err) {
    toast(err.message, 'err');
  }
}

/* ── device tab ─────────────────────────────────────────────────────────── */

async function refreshDevice() {
  const [info, stats] = await Promise.all([request('sys.info'), request('sys.stats')]);
  const rows = [
    ['Device', info.device],
    ['App', `${info.app} (${info.project})`],
    ['Schema', info.schema],
    ['ESP-IDF', info.idf],
    ['MTU', info.mtu],
    ['Uptime', `${Math.round((info.uptimeMs || 0) / 1000)} s`],
    ['Free heap', `${Math.round((info.freeHeap || 0) / 1024)} KiB`],
    ['Pins', `IR rx ${info.pins?.irRx} / tx ${info.pins?.irTx} / hotkey ${info.pins?.hotkey}`],
    ['Profiles', `${stats.profiles} of ${stats.maxProfiles}`],
    ['Commands', `${stats.commands} of ${stats.maxButtons}`],
    ['Storage', `${stats.storageBytes} B in SPIFFS`],
    ['Hotkey slots', stats.maxHotkeySteps],
  ];
  render($('#device-info'), rows.flatMap(([k, v]) => [
    el('dt', { text: k }),
    el('dd', { text: String(v) }),
  ]));
}

/* ── waveform drawing ───────────────────────────────────────────────────── */

function drawWaveform(canvas, data) {
  const dpr = window.devicePixelRatio || 1;
  const cssW = canvas.clientWidth || 320;
  const cssH = 62;
  canvas.width = Math.round(cssW * dpr);
  canvas.height = Math.round(cssH * dpr);

  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cssW, cssH);

  const durations = data.durations || [];
  const total = durations.reduce((a, b) => a + b, 0) || 1;
  const hi = 8;
  const lo = cssH - 8;

  // A demodulating receiver idles high, so the first level is the inverse of startLevel.
  let level = data.startLevel ? 0 : 1;
  let x = 0;

  ctx.lineWidth = 1;
  ctx.strokeStyle = '#2a3140';
  ctx.beginPath();
  ctx.moveTo(0, lo);
  ctx.lineTo(cssW, lo);
  ctx.stroke();

  ctx.strokeStyle = '#2f81f7';
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  ctx.moveTo(0, level ? hi : lo);
  for (const d of durations) {
    const w = Math.max(1, (d / total) * cssW);
    const y = level ? hi : lo;
    ctx.lineTo(x, y);
    ctx.lineTo(x + w, y);
    level ^= 1;
    ctx.lineTo(x + w, level ? hi : lo);
    x += w;
  }
  ctx.stroke();

  ctx.fillStyle = '#8b949e';
  ctx.font = '10px system-ui, sans-serif';
  ctx.fillText(`${data.edges ?? durations.length} edges · ${(total / 1000).toFixed(1)} ms`, 4, 11);
}

/* ── UI plumbing ────────────────────────────────────────────────────────── */

function showTab(name) {
  for (const btn of $('#tabs').children) {
    btn.classList.toggle('active', btn.dataset.tab === name);
  }
  for (const section of document.querySelectorAll('.tab')) {
    section.classList.toggle('active', section.id === `tab-${name}`);
  }
  if (name === 'hotkey' && state.device?.gatt?.connected) refreshHotkey().catch(() => {});
}

/** Run an action, surface failures as a toast, and surface the result on success. */
async function run(fn, okMessage) {
  try {
    const result = await fn();
    if (okMessage) toast(okMessage, 'ok');
    return result;
  } catch (err) {
    logLine('err', String(err.message || err));
    toast(String(err.message || err), 'err');
    return null;
  }
}

function askText(title, value) {
  const dialog = $('#modal');
  $('#modal-title').textContent = title;
  const input = $('#modal-input');
  input.value = value || '';
  dialog.showModal();
  input.focus();
  input.select();

  return new Promise((resolve) => {
    dialog.addEventListener('close', () => {
      resolve(dialog.returnValue === 'ok' ? input.value.trim() || null : null);
    }, { once: true });
  });
}

/* ── browser support banner ─────────────────────────────────────────────── */

/**
 * Explain precisely why the dashboard cannot reach the device, and what to do about it.
 *
 * The two failure modes look identical from the user's side but have different fixes, so telling
 * them apart matters: Chrome only exposes `navigator.bluetooth` in a *secure context*, and a LAN
 * address over plain HTTP is not one. That is the single most confusing way to lose an hour here.
 */
function updateSupportBanner() {
  const box = $('#unsupported');

  if (window.isSecureContext && navigator.bluetooth) {
    box.classList.add('hidden');
    return;
  }

  const code = (text) => el('code', { text });
  const title = $('#unsupported-title');
  const detail = $('#unsupported-detail');

  if (!window.isSecureContext) {
    title.textContent = 'This page is not a secure context.';
    render(detail,
      el('p', {}, 'Chrome only exposes Web Bluetooth over HTTPS or on localhost. ',
        'This page is loaded from ', code(location.origin),
        ' over plain HTTP, so Connect cannot work. Any one of these fixes it:'),
      el('ul', {},
        el('li', {}, 'Run ', code('adb reverse tcp:8000 tcp:8000'),
          ', then open ', code('http://localhost:8000'),
          ' — localhost counts as secure and needs no certificate.'),
        el('li', {}, 'Or add ', code(location.origin), ' to ',
          code('chrome://flags/#unsafely-treat-insecure-origin-as-secure'),
          ' and restart Chrome.'),
        el('li', {}, 'Or deploy this directory to GitHub Pages, which serves HTTPS.')));
  } else {
    title.textContent = 'Web Bluetooth is not available in this browser.';
    render(detail,
      el('p', {}, 'Use ', el('b', { text: 'Chrome on Android' }),
        '. Firefox on Android does not support Web Bluetooth at all.'));
  }

  box.classList.remove('hidden');
}

/* ── wiring ─────────────────────────────────────────────────────────────── */

$('#btn-connect').addEventListener('click', () => {
  if (state.device?.gatt?.connected) disconnect();
  else connect();
});

$('#tabs').addEventListener('click', (e) => {
  const btn = e.target.closest('button[data-tab]');
  if (btn) showTab(btn.dataset.tab);
});

$('#btn-add-profile').addEventListener('click', async () => {
  const input = $('#new-profile');
  const name = input.value.trim();
  if (!name) return toast('Enter a name', 'err');
  const data = await run(() => request('profile.create', { name }), 'Created');
  if (!data) return;
  input.value = '';
  state.cursorProfile = data.id;
  await refreshProfiles();
});

$('#profile-select').addEventListener('change', (e) => {
  state.cursorProfile = Number(e.target.value) || 0;
  renderProfiles();
  refreshButtons();
});

$('#btn-learn').addEventListener('click', startLearn);
$('#btn-learn-cancel').addEventListener('click', async () => {
  await run(() => request('button.learn_cancel'), 'Cancelled');
  setLearning(false);
});

$('#btn-hotkey-add').addEventListener('click', addHotkeyStep);
$('#btn-hotkey-test').addEventListener('click', testHotkey);

$('#btn-hotkey-save').addEventListener('click', async () => {
  if (!state.cursorProfile) return toast('Select a remote first', 'err');
  const data = await run(() => request('hotkey.set', {
    profileId: state.cursorProfile,
    steps: state.hotkey,
  }), 'Hotkey saved');
  if (data) await refreshHotkey();
});

$('#btn-hotkey-clear').addEventListener('click', async () => {
  if (!state.hotkey.length) return toast('Nothing to clear', 'err');
  if (!confirm('Clear the hotkey sequence on the device?')) return;
  await run(() => request('hotkey.clear'), 'Hotkey cleared');
  state.hotkey = [];
  renderHotkey();
});

$('#btn-refresh').addEventListener('click', () => run(refreshDevice));

$('#btn-factory-reset').addEventListener('click', async () => {
  if (!confirm('Erase every remote, button and command on the device? This cannot be undone.')) return;
  await run(() => request('sys.factory_reset'), 'Factory reset done');
  state.cursorProfile = 0;
  await refreshProfiles();
  await refreshButtons();
  await refreshDevice();
  await refreshHotkey();
});

$('#btn-log-clear').addEventListener('click', () => render($('#log-list')));

$('#btn-raw-send').addEventListener('click', async () => {
  let msg;
  try {
    msg = JSON.parse($('#raw-req').value);
  } catch {
    return toast('Not valid JSON', 'err');
  }
  if (!msg.cmd) return toast('Add a "cmd" field', 'err');
  await run(() => request(msg.cmd, msg.args));
});

/* keep the selected profile's buttons fresh when returning to the tab */
document.querySelector('button[data-tab="buttons"]').addEventListener('click', () => {
  if (state.device?.gatt?.connected) refreshButtons().catch(() => {});
});

updateSupportBanner();
setLink(false);
logLine('evt', 'dashboard ready — press Connect');
