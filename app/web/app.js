// The receiver's web app. Plain JS, no build step: the firmware serves these
// files gzipped from flash. It speaks the firmware's /api/v1 (docs/test-api.md;
// the calls only the app uses are listed in docs/app.md, "The API"). It reads
// GET info once (what the box is and can do: info.caps, so one app serves
// Lite, Plus and Ultra) and polls GET state; every write answers with the new
// state.
'use strict';

const POLL_MS = 2000;
const EQ_PRESETS = {
  Flat: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
  'Bass +': [6, 5, 3, 1, 0, 0, 0, 0, 0, 0],
  Loudness: [5, 4, 2, 0, -1, -1, 0, 2, 3, 3],
  Vocal: [-2, -1, 0, 1, 3, 3, 2, 1, 0, -1],
  'Treble -': [0, 0, 0, 0, 0, 0, -1, -2, -4, -5],
};
const SOURCES = { spotify: 'Spotify', airplay: 'AirPlay', bt: 'Bluetooth', lms: 'Lyrion', test: 'Test signal' };
const INPUTS = { stream: 'Streaming', dj_direct: 'DJ direct', line: 'Line in', optical: 'Optical' };
const INPUT_NOTES = {
  stream: 'Plays what you send from Spotify, AirPlay, Bluetooth or Lyrion.',
  dj_direct: 'The RCA line in goes straight to the amplifier with no delay, for DJ decks and turntables. ' +
    'Streaming, EQ and the sub settings do not apply.',
  line: 'The RCA line in through the digital path, so EQ and the sub settings apply. Adds a few milliseconds of delay.',
  optical: 'The optical (TOSLINK) input, through EQ and the sub settings.',
};
const IR_ACTIONS = { volup: 'Volume up', voldown: 'Volume down', mute: 'Mute', toggle: 'Play / pause',
  next: 'Next', prev: 'Previous', input: 'Input', power: 'Standby' };
// The firmware's self-test names (docs/test-api.md), as a person reads them.
const CHECKS = { pd_sink_i2c: 'Charger chip', pd_contract: 'Charger contract', vbus_matches: 'Supply voltage',
  codec_i2c: 'Audio chip', dac_i2c: 'DAC', pvdd_on: 'Amplifier supply', amp_no_fault: 'Amplifier',
  psram: 'Memory', heap: 'Free memory', network: 'Network' };

let I = null;          // GET info: what the box is
let S = null;          // the last GET state
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
  const r = await fetch('api/v1/' + path, opt);
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}

// set() posts a change and takes the state it answers with; a call whose
// reply isn't a state (the bench's amp/input/control) is followed by a read.
async function set(path, body) {
  try {
    const r = await api(path, body);
    take(r.audio ? r : await api('state'));
  } catch (e) { toast(e.message); }
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

function seg(options, current, pick) {
  return h('div.seg', {}, Object.entries(options).map(([v, label]) =>
    h('button', { className: String(current) === v ? 'on' : '', onclick: () => pick(v) }, label)));
}
const onOff = (cur, pick) => seg({ false: 'Off', true: 'On' }, !!cur, (v) => pick(v === 'true'));

function take(state) {
  if (!state || !state.audio) return;
  S = state;
  draw();
}

async function poll() {
  try {
    if (!I) I = await api('info');
    take(await api('state'));
    online = true;
  } catch { online = false; draw(); }
  $('#conn').classList.toggle('on', online);
  setTimeout(poll, POLL_MS);
}

function draw() {
  const typing = document.activeElement && /INPUT|SELECT|TEXTAREA/.test(document.activeElement.tagName) &&
    document.activeElement.type !== 'range';
  if (holding || typing) return;
  if (I) $('#name').textContent = I.name;
  for (const b of document.querySelectorAll('#tabs button')) b.classList.toggle('on', b.dataset.tab === tab);
  const view = $('#view');
  const y = window.scrollY;
  view.replaceChildren(...(S && I ? PAGES[tab]() : [offline()]).flat().filter((k) => k != null && k !== false));
  window.scrollTo(0, y);
}

function offline() {
  return h('div.card.err', {}, h('div.big', {}, 'Looking for the receiver…'),
    h('p.dim', {}, 'Check that it is powered and that this phone is on the same Wi-Fi. ' +
      'If it is new, join its own Wi-Fi network (named HiFi-…) and this page opens by itself.'));
}

// ---- Play ---------------------------------------------------------------

function powerNotice() {
  const p = S.power, a = S.amp, g = S.governor, c = p.contract || {};
  if (a.latched || a.fault) return h('div.card.err', {}, h('b', {}, 'Amplifier fault. '),
    'It has switched itself off to protect the speakers. Check the speaker wires for a short, then press Clear.',
    h('div.row', {}, h('button', { onclick: () => set('amp', { reset: true }) }, 'Clear')));
  if (p.state === 'wait_pd') return h('div.card.warn', {}, h('b', {}, 'Speakers off: charger too weak. '),
    `This port gives ${c.v ?? 5} V. Use a USB-C charger of 65 W or more` +
    (I.caps.pdo_v_max > 20 ? ` (a 140 W one with a 240 W cable for full power).` : '.'));
  if (p.state === 'no_pvdd') return h('div.card.err', {}, h('b', {}, 'Speakers off: the amplifier supply did not switch on. '),
    'Unplug the charger and plug it back in. If this comes back, the box needs service.');
  if (p.state === 'undervolt' || p.state === 'overvolt') return h('div.card.err', {},
    h('b', {}, 'Speakers off: the supply is out of range. '), `It reads ${p.vbus_v?.toFixed(1)} V. Try another charger or cable.`);
  if (g.limiting && g.gain_db < -0.5) return h('div.card.warn', {}, h('b', {}, 'Power limit. '),
    `Turned down ${(-g.gain_db).toFixed(1)} dB to stay within the charger's ${Math.round(c.w)} W.`);
  return null;
}

function pagePlay() {
  const a = S.audio, np = a.playing || {};
  const direct = a.input !== 'stream';
  const idle = direct || a.source === 'idle' || !np.title;
  const title = direct ? INPUTS[a.input] : idle ? 'Nothing playing' : np.title;
  const sub = direct ? 'Playing what is plugged into the RCA line in.'
    : idle ? `Play to “${I.name}” from Spotify, AirPlay, Bluetooth or Lyrion.`
      : [np.artist, np.album].filter(Boolean).join(' — ');
  return [
    powerNotice(),
    h('div.card', {},
      h('h2', {}, direct ? 'Input' : idle ? 'Ready' : `Playing from ${SOURCES[a.source] || a.source}`),
      h('div.np-title', {}, title), h('div.np-sub', {}, sub),
      !idle && h('div.row', { style: 'justify-content:center;margin-top:14px' },
        h('button.icon', { 'aria-label': 'Previous', onclick: () => set('control', { action: 'prev' }) }, '⏮'),
        h('button.icon.pri', { 'aria-label': np.state === 'play' ? 'Pause' : 'Play',
          onclick: () => set('control', { action: np.state === 'play' ? 'pause' : 'play' }) }, np.state === 'play' ? '⏸' : '▶'),
        h('button.icon', { 'aria-label': 'Next', onclick: () => set('control', { action: 'next' }) }, '⏭'))),
    volumeCard(),
    h('div.card', {}, h('h2', {}, 'Input'),
      seg(Object.fromEntries(['stream', ...I.caps.line_modes].map((m) => [m, INPUTS[m] || m])), a.input,
        (m) => set('input', { input: m })),
      h('p.dim', {}, INPUT_NOTES[a.input] || '')),
  ];
}

function volumeCard() {
  const a = S.audio;
  const num = h('span.big', {}, String(a.volume));
  return h('div.card', {}, h('h2', {}, 'Volume'),
    h('div.row', {},
      h('button.icon', { 'aria-label': a.mute ? 'Unmute' : 'Mute', onclick: () => set('volume', { mute: !a.mute }) },
        a.mute ? '🔇' : '🔊'),
      h('div.grow', {}, slider({ min: 0, max: a.max_volume, value: a.volume, 'aria-label': 'Volume',
        show: (v) => { num.textContent = v; } }, (v) => set('volume', { volume: v }))),
      num),
    I.caps.zones > 1 && zoneB());
}

function zoneB() {
  const z = S.audio.zone_b || {};
  return h('div', {},
    h('div.row', {}, h('span.grow', {}, 'Zone B speakers'), onOff(z.on, (on) => set('zone', { zone: 'b', on }))),
    z.on && z.volume != null && h('div.row', {}, h('span.dim', {}, 'Level'),
      h('div.grow', {}, slider({ min: 0, max: 100, value: z.volume }, (v) => set('zone', { zone: 'b', volume: v })))));
}

// ---- Sound --------------------------------------------------------------

function pageSound() {
  const a = S.audio, c = I.caps;
  const fmt = (f) => (f >= 1000 ? f / 1000 + 'k' : String(f));
  const gains = a.eq.slice();
  const preset = Object.keys(EQ_PRESETS).find((k) => EQ_PRESETS[k].every((g, i) => g === a.eq[i]));
  return [
    a.input === 'dj_direct' && h('div.card.warn', {}, 'DJ direct is on, so the line in skips the EQ and sub settings.'),
    h('div.card', {}, h('h2', {}, 'Equaliser'),
      h('div.chips', {}, Object.keys(EQ_PRESETS).map((k) => h('button', { className: preset === k ? 'on' : '',
        onclick: () => set('eq', { gains: EQ_PRESETS[k] }) }, k))),
      h('div.eq', { style: 'margin-top:14px' }, c.eq_bands.map((f, i) => h('label', {},
        h('span', {}, (gains[i] > 0 ? '+' : '') + gains[i]),
        slider({ min: c.eq_min, max: c.eq_max, step: 1, value: gains[i], 'aria-label': fmt(f) + ' Hz' },
          (v) => { gains[i] = v; set('eq', { gains }); }),
        fmt(f))))),
    subCard(),
    h('div.card', {}, h('h2', {}, 'Volume limit'),
      h('p.dim', {}, 'The highest the volume can go, from the app, buttons or remote. Turn it down to protect smaller speakers.'),
      h('div.row', {}, h('div.grow', {}, slider({ min: 30, max: 100, value: a.max_volume, 'aria-label': 'Volume limit',
        show: (v) => { $('#maxv').textContent = v; } }, (v) => set('volume', { max_volume: v }))),
      h('span.big', { id: 'maxv' }, String(a.max_volume)))),
  ];
}

function subCard() {
  const s = S.audio.sub, c = I.caps.sub;
  const line = (label, attrs, value, unit, send) => {
    const v = h('span.dim', { style: 'min-width:64px;text-align:right' }, value + unit);
    return h('div.row', {}, h('span', { style: 'min-width:84px' }, label),
      h('div.grow', {}, slider({ ...attrs, value, show: (x) => { v.textContent = x + unit; } }, send)), v);
  };
  const rows = [h('div.row', {}, h('span.grow', {}, 'Sub outputs'), onOff(s.on, (on) => set('sub', { on })))];
  if (s.on) {
    rows.push(line('Level', { min: -30, max: 6, step: 0.5 }, s.level_db, ' dB', (x) => set('sub', { level_db: x })));
    if (c.crossover) {
      rows.push(line('Crossover', { min: 40, max: 160, step: 5 }, s.xover_hz, ' Hz', (x) => set('sub', { xover_hz: x })));
      rows.push(h('div.row', {}, h('span.grow', {}, 'Take the deep bass off the main speakers'),
        onOff(s.mains_hp, (v) => set('sub', { mains_hp: v }))));
    }
    if (c.per_sub) {
      (s.subs || []).forEach((one, i) => {
        const put = (k) => (x) => set('sub', { sub: i, [k]: x });
        rows.push(h('h2', { style: 'margin-top:16px' }, `Sub ${i + 1}`),
          line('Trim', { min: -12, max: 6, step: 0.5 }, one.trim_db, ' dB', put('trim_db')),
          line('Delay', { min: 0, max: 20, step: 0.1 }, one.delay_ms, ' ms', put('delay_ms')),
          h('div.row', {}, h('span.grow', {}, 'Phase'),
            seg({ false: '0°', true: '180°' }, !!one.invert, (v) => put('invert')(v === 'true'))));
      });
    }
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
    wifiCard(), nameCard(), remoteCard(), updateCard(), diagCard(),
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
  const j = n.join;
  return h('div.card', {}, h('h2', {}, 'Wi-Fi'),
    n.mode === 'sta' && h('div.row', {}, h('span.grow', {}, h('b', {}, n.ssid), h('div.dim', {},
      `${n.ip} · signal ${bars(n.rssi)}` + (n.eth ? ' · Ethernet connected' : ''))),
      h('button', { onclick: () => confirm(`Forget ${n.ssid}? The receiver goes back to its own HiFi-… network.`) &&
        set('wifi/forget', {}) }, 'Forget')),
    j && h('p', { className: j.state === 'failed' ? 'bad' : 'dim' },
      j.state === 'failed' ? `Could not join ${j.ssid}: ${j.reason || 'wrong password?'}`
        : j.state === 'ok' ? `Joined ${j.ssid}. Reconnect this phone to it and open http://${n.hostname}.local (${j.ip}).`
          : `Joining ${j.ssid}…`),
    h('ul.list', {}, nets.map((w) => h('li', {}, h('span.grow', {}, w.ssid, ' ', h('span.dim', {}, bars(w.rssi) + (w.secure ? ' 🔒' : ''))),
      h('button', { onclick: () => { other.value = w.ssid; pw.focus(); } }, 'Choose')))),
    h('div.row', {}, h('button', { onclick: async () => {
      toast('Scanning…');
      try { scan = (await api('wifi/scan')).networks; draw(); } catch (e) { toast(e.message); }
    } }, nets.length ? 'Scan again' : 'Scan for networks')),
    h('div.row', {}, other), h('div.row', {}, pw),
    h('div.row', {}, h('button.pri', { onclick: () => join(other.value.trim()) }, 'Join')),
    h('p.dim', {}, '2.4 GHz networks only.'));
}

function bars(rssi) {
  return rssi > -55 ? '▂▄▆█' : rssi > -67 ? '▂▄▆' : rssi > -78 ? '▂▄' : '▂';
}

function nameCard() {
  const inp = h('input', { type: 'text', value: I.name, maxLength: 32 });
  return h('div.card', {}, h('h2', {}, 'Name'),
    h('p.dim', {}, 'What Spotify, AirPlay and Bluetooth show this receiver as. Bluetooth: pick this name on your ' +
      'phone; the receiver can be found whenever no other phone is connected to it.'),
    h('div.row', {}, h('div.grow', {}, inp), h('button', { onclick: async () => {
      await set('name', { name: inp.value.trim() });
      I = await api('info').catch(() => I); draw(); toast('Renamed');
    } }, 'Save')));
}

function remoteCard() {
  const ir = S.ir || { learning: null, learned: [] };
  return h('div.card', {}, h('h2', {}, 'Remote'),
    h('p.dim', {}, ir.learning ? `Press the button on your remote for “${IR_ACTIONS[ir.learning]}” now…`
      : 'Any TV or audio remote works. Tap Teach, then press the remote button you want for it.'),
    h('ul.list', {}, Object.entries(IR_ACTIONS).map(([k, label]) => h('li', {},
      h('span.grow', {}, label, ' ', h('span', { className: ir.learned.includes(k) ? 'ok' : 'dim' },
        ir.learned.includes(k) ? '✓' : '')),
      h('button', { className: ir.learning === k ? 'pri' : '', onclick: () => set('ir/learn', { action: k }) },
        ir.learning === k ? 'Waiting…' : 'Teach')))),
    ir.learned.length > 0 && h('div.row', {}, h('button', {
      onclick: () => confirm('Forget every remote button?') && set('ir/forget', {}) }, 'Forget all')));
}

function updateCard() {
  const u = S.update || { state: 'idle' };
  const file = h('input', { type: 'file', accept: '.bin', hidden: true, onchange: () => upload(file.files[0]) });
  const busy = u.state === 'downloading' || u.state === 'flashing';
  const newer = check && check.latest && check.latest !== I.fw;
  return h('div.card', {}, h('h2', {}, 'Updates'),
    h('div.dim', {}, `Firmware ${I.fw}`),
    busy && [h('p', {}, u.state === 'downloading' ? 'Downloading…' : 'Installing… do not unplug.'),
      h('div.bar', {}, h('i', { style: `width:${u.progress || 0}%` }))],
    u.state === 'done' && h('p.ok', {}, 'Updated. The receiver is restarting.'),
    u.state === 'error' && h('p.bad', {}, `Update failed: ${u.error || 'unknown error'}. The old firmware is still in place.`),
    check && h('p', { className: newer ? '' : 'dim' }, newer ? `Version ${check.latest} is available. ${check.notes || ''}` : 'Up to date.'),
    !busy && h('div.row', {},
      newer ? h('button.pri', { onclick: () => set('update', { url: check.url }) }, 'Install')
        : check !== false && h('button', { onclick: async () => {
          try { check = await api('update/check'); } catch { check = false; toast('No update server: use a file'); }
          draw();
        } }, 'Check for updates'),
      h('button', { onclick: () => file.click() }, 'From a file…'), file));
}

function upload(f) {
  if (!f) return;
  const x = new XMLHttpRequest();
  x.open('POST', 'api/v1/update/upload');
  x.setRequestHeader('Content-Type', 'application/octet-stream');
  x.upload.onprogress = (e) => {
    if (e.lengthComputable && S) { S.update = { state: 'downloading', progress: Math.round(e.loaded * 100 / e.total) }; draw(); }
  };
  x.onload = () => { try { take(JSON.parse(x.responseText)); } catch { /* the next poll shows it */ } };
  x.onerror = () => toast('Upload failed');
  x.send(f);
}

function diagCard() {
  const p = S.power, c = p.contract || {};
  const kv = (k, v) => h('li', {}, h('span.grow.dim', {}, k), h('span', {}, v));
  return h('div.card', {}, h('h2', {}, 'About and diagnostics'),
    h('ul.list', {},
      kv('Model', I.model), kv('Firmware', I.fw), kv('Address', `${S.net.ip || '—'} · ${I.mac}`),
      kv('Charger', c.protocol && c.protocol !== 'none'
        ? `${c.v} V` + (c.i_max_a ? ` × ${c.i_max_a} A (${Math.round(c.w)} W)` : '') + ` · ${c.protocol.toUpperCase()}` : 'none'),
      kv('Supply now', p.vbus_v != null ? `${p.vbus_v.toFixed(1)} V` : '—'),
      kv('Amplifier', S.amp.latched || S.amp.fault ? 'fault' : p.state !== 'on' ? 'off' : S.amp.enabled ? 'on' : 'held off'),
      S.amp.temp_c != null && kv('Temperature', `${S.amp.temp_c} °C`),
      kv('Up', uptime(S.uptime_s))),
    selftest && h('ul.list', { style: 'margin-top:10px' }, selftest.checks.map((t) => h('li', {},
      h('span', { className: t.pass ? 'ok' : 'bad' }, t.pass ? '✓' : '✗'),
      h('span.grow', {}, CHECKS[t.name] || t.name), h('span.dim', {}, t.value ?? '')))),
    h('div.row', {}, h('button', { onclick: async () => {
      toast('Running self-test…');
      try { selftest = await api('selftest', {}); draw(); } catch (e) { toast(e.message); }
    } }, 'Run self-test'),
    h('button', { onclick: () => confirm('Restart the receiver?') && set('reboot', {}).then(() => toast('Restarting…')) }, 'Restart'),
    h('button', { onclick: () => confirm('Erase Wi-Fi, name, remote and sound settings and start again from setup?') &&
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
