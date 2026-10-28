// The receiver's web app. Plain JS, no build step: the firmware serves these
// files gzipped from flash. Everything it shows comes from GET /api/state and
// every control is one POST that answers with the new state (docs/test-api.md,
// "The app's API"). What a board can do comes from state.caps, so one app
// serves Lite, Plus and Ultra.
'use strict';

const POLL_MS = 2000;
const EQ_PRESETS = {
  Flat: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
  'Bass +': [6, 5, 3, 1, 0, 0, 0, 0, 0, 0],
  Loudness: [5, 4, 2, 0, -1, -1, 0, 2, 3, 3],
  Vocal: [-2, -1, 0, 1, 3, 3, 2, 1, 0, -1],
  'Treble -': [0, 0, 0, 0, 0, 0, -1, -2, -4, -5],
};
const SOURCE_NAMES = { spotify: 'Spotify', airplay: 'AirPlay', bt: 'Bluetooth',
  lms: 'Lyrion', line: 'Line in', optical: 'Optical', idle: 'Nothing' };

let S = null;          // the last state the receiver sent
let tab = 'play';
let online = false;
let holding = false;   // a slider is under a finger: don't redraw over it
let scan = null;       // Wi-Fi networks from the last scan
let selftest = null;   // the last self-test result
let check = null;      // the last online update check

const $ = (sel, el = document) => el.querySelector(sel);

// h('div.card', {onclick}, child, ...) -> element
function h(tag, attrs, ...kids) {
  const [name, ...cls] = tag.split('.');
  const el = document.createElement(name);
  if (cls.length) el.className = cls.join(' ');
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else if (k in el && k !== 'list') el[k] = v;
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const k of kids.flat()) if (k != null && k !== false) el.append(k);
  return el;
}

function toast(msg) {
  const t = $('#toast');
  t.textContent = msg; t.hidden = false;
  clearTimeout(toast.t); toast.t = setTimeout(() => { t.hidden = true; }, 3000);
}

async function api(path, body) {
  const opt = body === undefined ? {} : {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) };
  const r = await fetch('api/' + path, opt);
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}

// set() posts a change and takes the state it answers with.
async function set(path, body) {
  try { take(await api(path, body)); } catch (e) { toast(e.message); }
}

// A slider sends while it moves, at most every 150 ms, and once when let go.
function throttled(fn, ms = 150) {
  let last = 0, timer = null, args = null;
  return (...a) => {
    args = a; clearTimeout(timer);
    const wait = last + ms - Date.now();
    if (wait <= 0) { last = Date.now(); fn(...args); } else timer = setTimeout(() => { last = Date.now(); fn(...args); }, wait);
  };
}

function slider({ show, ...attrs }, send) {
  const out = throttled(send);
  return h('input', { type: 'range', ...attrs,
    onpointerdown: () => { holding = true; },
    onpointerup: () => { holding = false; },
    onpointercancel: () => { holding = false; },
    oninput: (e) => { out(+e.target.value); if (show) show(+e.target.value); },
    onchange: (e) => { holding = false; send(+e.target.value); } });
}

function take(state) {
  if (!state || !state.audio) return;
  S = state;
  draw();
}

async function poll() {
  try { take(await api('state')); online = true; } catch { online = false; draw(); }
  $('#conn').classList.toggle('on', online);
  setTimeout(poll, POLL_MS);
}

function draw() {
  const typing = document.activeElement && /INPUT|SELECT|TEXTAREA/.test(document.activeElement.tagName) &&
    document.activeElement.type !== 'range';
  if (holding || typing) return;
  if (S) $('#name').textContent = S.device.name;
  for (const b of document.querySelectorAll('#tabs button')) b.classList.toggle('on', b.dataset.tab === tab);
  const view = $('#view');
  const y = window.scrollY;
  view.replaceChildren(...(S ? PAGES[tab]() : [offline()]));
  window.scrollTo(0, y);
}

function offline() {
  return h('div.card.err', {}, h('div.big', {}, 'Looking for the receiver…'),
    h('p.dim', {}, 'Check that it is powered and that this phone is on the same Wi-Fi. ' +
      'If it is new, join its own Wi-Fi network (named HiFi-…) and this page opens by itself.'));
}

// ---- Play ---------------------------------------------------------------

function powerNotice() {
  const p = S.power;
  if (p.amp_fault) return h('div.card.err', {}, h('b', {}, 'Amplifier fault. '),
    'It has switched itself off to protect the speakers. Check the speaker wires for a short, then press Clear.',
    h('div.row', {}, h('button', { onclick: () => set('amp', { clear_fault: true }) }, 'Clear')));
  if (!p.amp_on && p.pdo_v < 9) return h('div.card.warn', {}, h('b', {}, 'Speakers off: charger too weak. '),
    `This port gives ${p.pdo_v} V. Use a USB-C charger of 65 W or more` +
    (S.caps.pdo_v_max > 20 ? ` (${S.caps.pdo_v_max} V / 140 W for full power).` : '.'));
  if (p.limit_db > 0.5) return h('div.card.warn', {}, h('b', {}, 'Power limit. '),
    `Turned down ${p.limit_db.toFixed(1)} dB to stay within the charger's ${p.pdo_v * p.pdo_a} W.`);
  return null;
}

function pagePlay() {
  const a = S.audio, np = S.playing || {};
  const src = SOURCE_NAMES[a.source] || a.source;
  const idle = a.source === 'idle' || !np.title;
  return [
    powerNotice(),
    h('div.card', {},
      h('h2', {}, idle ? 'Ready' : `Playing from ${src}`),
      h('div.np-title', {}, idle ? 'Nothing playing' : np.title),
      h('div.np-sub', {}, idle ? 'Play from Spotify, AirPlay, Bluetooth or Lyrion, or choose Line in below.'
        : [np.artist, np.album].filter(Boolean).join(' — ')),
      S.caps.transport.includes(a.source) && h('div.row', { style: 'justify-content:center;margin-top:14px' },
        h('button.icon', { 'aria-label': 'Previous', onclick: () => set('transport', { cmd: 'prev' }) }, '⏮'),
        np.state === 'play'
          ? h('button.icon.pri', { 'aria-label': 'Pause', onclick: () => set('transport', { cmd: 'pause' }) }, '⏸')
          : h('button.icon.pri', { 'aria-label': 'Play', onclick: () => set('transport', { cmd: 'play' }) }, '▶'),
        h('button.icon', { 'aria-label': 'Next', onclick: () => set('transport', { cmd: 'next' }) }, '⏭'))),
    volumeCard(),
    h('div.card', {}, h('h2', {}, 'Source'),
      h('div.chips', {}, S.caps.sources.map((s) => h('button', { className: a.source === s ? 'on' : '',
        onclick: () => set('source', { source: s }) }, SOURCE_NAMES[s] || s)))),
    a.source === 'line' && lineCard(),
  ];
}

function volumeCard() {
  const a = S.audio;
  const num = h('span.big', {}, String(a.volume));
  return h('div.card', {}, h('h2', {}, 'Volume'),
    h('div.row', {},
      h('button.icon', { 'aria-label': a.mute ? 'Unmute' : 'Mute', onclick: () => set('mute', { mute: !a.mute }) },
        a.mute ? '🔇' : '🔊'),
      h('div.grow', {}, slider({ min: 0, max: 100, value: a.volume, 'aria-label': 'Volume',
        show: (v) => { num.textContent = v; } }, (v) => set('volume', { volume: v }))),
      num),
    S.caps.zones > 1 && zoneB());
}

function zoneB() {
  const z = S.audio.zone_b || {};
  return h('div', {},
    h('div.row', {}, h('span.grow', {}, 'Zone B speakers'),
      h('div.seg', {}, ['Off', 'On'].map((t, i) => h('button', { className: !!z.on === !!i ? 'on' : '',
        onclick: () => set('zone', { zone: 'b', on: !!i }) }, t)))),
    z.on && h('div.row', {}, h('span.dim', {}, 'Level'),
      h('div.grow', {}, slider({ min: 0, max: 100, value: z.volume }, (v) => set('zone', { zone: 'b', volume: v })))));
}

function lineCard() {
  const a = S.audio, modes = S.caps.line_modes;
  return h('div.card', {}, h('h2', {}, 'Line in'),
    modes.length > 1 && h('div.seg', {}, modes.map((m) => h('button', { className: a.line_mode === m ? 'on' : '',
      onclick: () => set('line', { mode: m }) }, m === 'direct' ? 'DJ direct' : 'Through EQ'))),
    h('p.dim', {}, a.line_mode === 'direct'
      ? 'Straight to the amplifier with no delay, for DJ decks and turntables. EQ and sub settings do not apply.'
      : 'Through the digital path, so EQ and the sub settings apply. Adds a few milliseconds of delay.'));
}

// ---- Sound --------------------------------------------------------------

function pageSound() {
  const a = S.audio, bands = S.caps.eq_bands;
  const fmt = (f) => (f >= 1000 ? f / 1000 + 'k' : String(f));
  const gains = a.eq.slice();
  const sendEq = () => set('eq', { gains });
  const preset = Object.keys(EQ_PRESETS).find((k) => EQ_PRESETS[k].every((g, i) => g === a.eq[i]));
  return [
    h('div.card', {}, h('h2', {}, 'Equaliser'),
      h('div.chips', {}, Object.keys(EQ_PRESETS).map((k) => h('button', { className: preset === k ? 'on' : '',
        onclick: () => set('eq', { gains: EQ_PRESETS[k] }) }, k))),
      h('div.eq', { style: 'margin-top:14px' }, bands.map((f, i) => h('label', {},
        h('span', {}, (gains[i] > 0 ? '+' : '') + gains[i]),
        slider({ min: S.caps.eq_min, max: S.caps.eq_max, step: 1, value: gains[i], 'aria-label': fmt(f) + ' Hz' },
          (v) => { gains[i] = v; sendEq(); }),
        fmt(f))))),
    subCard(),
  ];
}

function subCard() {
  const s = S.audio.sub, c = S.caps.sub;
  const send = (k) => (v) => set('sub', { [k]: v });
  const rows = [
    h('div.row', {}, h('span.grow', {}, 'Sub outputs'),
      h('div.seg', {}, ['Off', 'On'].map((t, i) => h('button', { className: !!s.on === !!i ? 'on' : '',
        onclick: () => set('sub', { on: !!i }) }, t)))),
  ];
  const line = (label, attrs, key, unit) => {
    const v = h('span.dim', { style: 'min-width:64px;text-align:right' }, s[key] + unit);
    return h('div.row', {}, h('span', { style: 'min-width:84px' }, label),
      h('div.grow', {}, slider({ ...attrs, value: s[key], show: (x) => { v.textContent = x + unit; } }, send(key))), v);
  };
  if (s.on) {
    rows.push(line('Level', { min: -15, max: 10, step: 0.5 }, 'db', ' dB'));
    if (c.crossover) rows.push(line('Crossover', { min: 40, max: 160, step: 5 }, 'xover_hz', ' Hz'));
    if (c.delay) rows.push(line('Delay', { min: 0, max: 20, step: 0.1 }, 'delay_ms', ' ms'));
    if (c.phase) rows.push(h('div.row', {}, h('span.grow', {}, 'Phase'),
      h('div.seg', {}, ['0°', '180°'].map((t, i) => h('button', { className: !!s.invert === !!i ? 'on' : '',
        onclick: () => set('sub', { invert: !!i }) }, t)))));
    if (c.mains_hp) rows.push(h('div.row', {}, h('span.grow', {}, 'Take the deep bass off the main speakers'),
      h('div.seg', {}, ['Off', 'On'].map((t, i) => h('button', { className: !!s.mains_hp === !!i ? 'on' : '',
        onclick: () => set('sub', { mains_hp: !!i }) }, t)))));
  }
  return h('div.card', {}, h('h2', {}, 'Subwoofers'), rows,
    !c.crossover && h('p.dim', {}, 'Full-range signal: set the crossover on the subwoofer itself.'));
}

// ---- Setup --------------------------------------------------------------

function pageSetup() {
  const n = S.net;
  return [
    n.mode === 'ap' && h('div.card.warn', {}, h('b', {}, 'Welcome. '),
      'Choose your home Wi-Fi below. The receiver joins it and you then find it at ',
      h('b', {}, `http://${n.hostname}.local`), '.'),
    wifiCard(), nameCard(), updateCard(), diagCard(),
  ];
}

function wifiCard() {
  const n = S.net;
  const pw = h('input', { type: 'password', placeholder: 'Wi-Fi password', autocomplete: 'off' });
  const other = h('input', { type: 'text', placeholder: 'Network name', autocapitalize: 'off' });
  const join = (ssid) => {
    if (!ssid) return toast('Choose a network');
    set('wifi', { ssid, password: pw.value }).then(() => toast(`Joining ${ssid}…`));
  };
  const nets = scan || [];
  return h('div.card', {}, h('h2', {}, 'Wi-Fi'),
    n.mode === 'sta' && h('div.row', {}, h('span.grow', {}, h('b', {}, n.ssid), h('div.dim', {},
      `${n.ip} · signal ${bars(n.rssi)}` + (S.caps.eth ? (n.eth ? ' · Ethernet connected' : '') : ''))),
      h('button', { onclick: () => set('wifi/forget', {}) }, 'Forget')),
    n.join && h('p', { className: n.join.state === 'failed' ? 'bad' : 'dim' },
      n.join.state === 'failed' ? `Could not join ${n.join.ssid}: ${n.join.reason || 'wrong password?'}`
        : n.join.state === 'ok' ? `Joined ${n.join.ssid}. Reconnect this phone to it and open http://${n.hostname}.local (${n.join.ip}).`
          : `Joining ${n.join.ssid}…`),
    h('ul.list', {}, nets.map((w) => h('li', {}, h('span.grow', {}, w.ssid, ' ', h('span.dim', {}, bars(w.rssi) + (w.secure ? ' 🔒' : ''))),
      h('button', { onclick: () => { other.value = w.ssid; pw.focus(); } }, 'Choose')))),
    h('div.row', {}, h('button', { onclick: async () => {
      toast('Scanning…');
      try { scan = await api('wifi/scan'); draw(); } catch (e) { toast(e.message); }
    } }, nets.length ? 'Scan again' : 'Scan for networks')),
    h('div.row', {}, other), h('div.row', {}, pw),
    h('div.row', {}, h('button.pri', { onclick: () => join(other.value.trim()) }, 'Join')));
}

function bars(rssi) {
  return rssi > -55 ? '▂▄▆█' : rssi > -67 ? '▂▄▆' : rssi > -78 ? '▂▄' : '▂';
}

function nameCard() {
  const inp = h('input', { type: 'text', value: S.device.name, maxLength: 32 });
  return h('div.card', {}, h('h2', {}, 'Name'),
    h('p.dim', {}, 'What Spotify, AirPlay and Bluetooth show this receiver as.'),
    h('div.row', {}, h('div.grow', {}, inp),
      h('button', { onclick: () => set('name', { name: inp.value.trim() }).then(() => toast('Renamed')) }, 'Save')));
}

function updateCard() {
  const u = S.update || { state: 'idle' };
  const file = h('input', { type: 'file', accept: '.bin', hidden: true, onchange: () => upload(file.files[0]) });
  const busy = u.state === 'downloading' || u.state === 'flashing';
  return h('div.card', {}, h('h2', {}, 'Updates'),
    h('div.dim', {}, `Firmware ${S.device.fw}`),
    busy && [h('p', {}, u.state === 'downloading' ? 'Downloading…' : 'Installing… do not unplug.'),
      h('div.bar', {}, h('i', { style: `width:${u.progress || 0}%` }))],
    u.state === 'done' && h('p.ok', {}, 'Updated. The receiver is restarting.'),
    u.state === 'error' && h('p.bad', {}, `Update failed: ${u.error || 'unknown error'}. The old firmware is still in place.`),
    check && h('p', { className: check.latest && check.latest !== S.device.fw ? '' : 'dim' },
      check.latest && check.latest !== S.device.fw ? `Version ${check.latest} is available. ${check.notes || ''}` : 'Up to date.'),
    !busy && h('div.row', {},
      check && check.latest && check.latest !== S.device.fw
        ? h('button.pri', { onclick: () => set('update', { url: check.url }) }, 'Install')
        : h('button', { onclick: async () => {
          try { check = await api('update/check'); draw(); } catch (e) { toast('No update server: ' + e.message); }
        } }, 'Check for updates'),
      h('button', { onclick: () => file.click() }, 'From a file…'), file));
}

function upload(f) {
  if (!f) return;
  const x = new XMLHttpRequest();
  x.open('POST', 'api/update/upload');
  x.setRequestHeader('Content-Type', 'application/octet-stream');
  x.upload.onprogress = (e) => { if (e.lengthComputable && S) { S.update = { state: 'downloading', progress: Math.round(e.loaded * 100 / e.total) }; draw(); } };
  x.onload = () => { try { take(JSON.parse(x.responseText)); } catch { /* the next poll shows it */ } };
  x.onerror = () => toast('Upload failed');
  x.send(f);
}

function diagCard() {
  const p = S.power, d = S.device;
  const kv = (k, v) => h('li', {}, h('span.grow.dim', {}, k), h('span', {}, v));
  return h('div.card', {}, h('h2', {}, 'About and diagnostics'),
    h('ul.list', {},
      kv('Model', d.model_name || d.model), kv('Firmware', d.fw), kv('Address', `${S.net.ip || '—'} · ${d.mac}`),
      kv('Charger', `${p.pdo_v} V × ${p.pdo_a} A (${Math.round(p.pdo_v * p.pdo_a)} W)`),
      kv('Supply now', p.vbus_v != null ? `${p.vbus_v.toFixed(1)} V` : '—'),
      kv('Amplifier', p.amp_fault ? 'fault' : p.amp_on ? 'on' : 'off'),
      p.temp_c != null && kv('Temperature', `${p.temp_c} °C`),
      kv('Up', uptime(d.uptime_s))),
    selftest && h('ul.list', { style: 'margin-top:10px' }, selftest.items.map((t) => h('li', {},
      h('span', { className: t.ok ? 'ok' : 'bad' }, t.ok ? '✓' : '✗'),
      h('span.grow', {}, t.name, t.detail && h('div.dim', {}, t.detail)), h('span.dim', {}, t.value ?? '')))),
    h('div.row', {}, h('button', { onclick: async () => {
      toast('Running self-test…');
      try { selftest = await api('selftest', {}); draw(); } catch (e) { toast(e.message); }
    } }, 'Run self-test'),
    h('button', { onclick: () => confirm('Restart the receiver?') && set('reboot', {}).then(() => toast('Restarting…')) }, 'Restart'),
    h('button', { onclick: () => confirm('Erase Wi-Fi, name and sound settings and start again from setup?') &&
      set('reset', { factory: true }).then(() => toast('Reset. Join the HiFi-… Wi-Fi to set it up again.')) }, 'Factory reset')));
}

function uptime(s) {
  if (s == null) return '—';
  const d = Math.floor(s / 86400), hr = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  return (d ? d + 'd ' : '') + (d || hr ? hr + 'h ' : '') + m + 'm';
}

const PAGES = { play: pagePlay, sound: pageSound, setup: pageSetup };

for (const b of document.querySelectorAll('#tabs button')) {
  b.addEventListener('click', () => { tab = b.dataset.tab; draw(); window.scrollTo(0, 0); });
}
const want = location.hash.slice(1);
if (PAGES[want]) tab = want;
api('state').then((s) => { if (s.net && s.net.mode === 'ap' && !PAGES[want]) tab = 'setup'; }).catch(() => {}).finally(poll);
if ('serviceWorker' in navigator && window.isSecureContext) navigator.serviceWorker.register('sw.js').catch(() => {});
