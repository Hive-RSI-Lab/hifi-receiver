// The receiver's web app. Plain JS, no build step: the firmware serves these
// files gzipped from flash. It speaks the firmware's /api/v1 (docs/test-api.md;
// the calls only the app uses are listed in docs/app.md, "The API"). It reads
// GET info once (what the box is and can do: info.caps, so one app serves
// Lite, Plus and Ultra) and polls GET state; every write answers with the new
// state. The Speakers page reaches the other receivers on the Wi-Fi too: it
// lists them from GET peers and talks to each at its own address, which the
// firmware allows from a LAN page (docs/app.md, "Speakers").
'use strict';

const POLL_MS = 2000;
// A poll gives up after this: a board that restarts mid-request (an update)
// never answers it, and without a limit the page waited on the browser's own
// timeout, minutes, before it noticed the receiver was back.
const POLL_WAIT_MS = 5000;
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
  line: 'The RCA line in through the digital path, so the volume, EQ and the sub settings apply. Adds about 0.1 s of delay.',
  optical: 'The optical (TOSLINK) input, through EQ and the sub settings.',
};
// the Play card while an input plays
const DIRECT_NOTES = { dj_direct: 'The RCA line in, straight to the amplifier.',
  line: 'The RCA line in, through the EQ and the sub settings.', optical: 'The optical (TOSLINK) input.' };
const IR_ACTIONS = { volup: 'Volume up', voldown: 'Volume down', mute: 'Mute', toggle: 'Play / pause',
  next: 'Next', prev: 'Previous', input: 'Input', power: 'Standby' };
// The firmware's self-test names (docs/test-api.md), as a person reads them.
const CHECKS = { pd_sink_i2c: 'Charger chip', pd_contract: 'Charger contract', vbus_matches: 'Supply voltage',
  codec_i2c: 'Audio chip', dac_i2c: 'DAC', pvdd_on: 'Amplifier supply', amp_no_fault: 'Amplifier', loopback: 'Speaker path',
  psram: 'Memory', heap: 'Free memory', network: 'Network' };

let I = null;          // GET info: what the box is
let S = null;          // the last GET state
let tab = 'speakers';
let online = false;
let slow = 0;          // polls in a row that ran out of time: the board answers one request at a time, so a long one (an update check) holds the rest
let uploading = false; // a firmware file is going up: the board answers nothing else until it is written
let holding = false;   // a slider is under a finger: don't redraw over it
let scan = null;       // Wi-Fi networks from the last scan
let selftest = null;   // the last self-test result
let P;                 // GET party, read on the Play page; null when the build has no party mode
let check = null;      // the last online update check: the reply, or {error} when it failed
let joining = null;    // {ssid, at, was}: the Wi-Fi join this page asked for, and the network it was on
const JOIN_WAIT_MS = 45000; // a join that hasn't landed by then has failed
const JOIN_SAME_MS = 10000; // re-joining the network it is on: on it this long after asking counts as joined
const JOIN_KEEP_MS = 300000; // then the page forgets the join

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

function toast(msg, ms = 3000) {
  const t = $('#toast');
  t.textContent = msg; t.hidden = false;
  clearTimeout(toast.t); toast.t = setTimeout(() => { t.hidden = true; }, ms);
}

// base: '' for this receiver, 'http://<ip>/' for another one. A POST to
// another one goes as text/plain, a CORS "simple" request with no preflight
// (the firmware reads the JSON body whatever its type).
async function api(path, body, waitMs, base = '') {
  const opt = body === undefined ? {} : {
    method: 'POST', headers: { 'Content-Type': base ? 'text/plain' : 'application/json' }, body: JSON.stringify(body) };
  // AbortController, not AbortSignal.timeout: that one is iOS 16+ only
  const c = waitMs ? new AbortController() : null;
  const t = c && setTimeout(() => c.abort(), waitMs);
  if (c) opt.signal = c.signal;
  try {
    const r = await fetch(base + 'api/v1/' + path, opt);
    // a body cut off by the limit is no answer, not an empty one
    const j = await r.json().catch((e) => { if (e.name === 'AbortError') throw e; return {}; });
    if (!r.ok) throw Object.assign(new Error(j.error || r.statusText), { status: r.status });
    return j;
  } finally { clearTimeout(t); }
}

// set() posts a change and takes the state it answers with; a call whose
// reply isn't a state (the bench's amp/input/control) is followed by a read.
// It answers true once the receiver took the change; a refusal is toasted.
async function set(path, body) {
  try {
    const r = await api(path, body);
    take(r.audio ? r : await api('state').catch(() => null));
    return true;
  } catch (e) { toast(e.message); return false; }
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
  remember(state);
  draw();
}

// ---- Power history ------------------------------------------------------
// The firmware keeps no history: the app keeps the last ~15 minutes of polled
// samples in memory and graphs them (Play: a short graph, Setup: the full one).
const HIST_N = 450;    // 15 min at POLL_MS
const H = [];          // [t ms, in W, out W, budget W, VBUS V, RSSI dBm, free heap B, limiting 0/1]

function remember(st) {
  const g = st.governor || {}, t = Date.now();
  if (g.in_w == null || (H.length && t - H[H.length - 1][0] < POLL_MS / 2)) return;  // a write's reply right after a poll
  H.push([t, g.in_w, g.out_w, g.budget_w, st.power && st.power.vbus_v, st.net && st.net.rssi,
    st.sys && st.sys.heap_free, g.limiting ? 1 : 0]);
  if (H.length > HIST_N) H.shift();
}

// The graphs are plain SVG built as strings of numbers. x is time, so a gap in
// the polls (a phone asleep) stays a gap; y is clamped to lo..hi.
const PLAY_SPAN = 300e3, DIAG_SPAN = 900e3;
const niceTop = (v) => [5, 10, 15, 20, 30, 40, 60, 80, 100, 150, 200, 300].find((s) => s >= v) || 400;
const yOf = (v, lo, hi, ht) => (ht - 1 - (Math.max(lo, Math.min(hi, v)) - lo) / (hi - lo) * (ht - 2)).toFixed(1);
const level = (v, lo, hi, w, ht) => (v != null && v > lo && v < hi ? `M0 ${yOf(v, lo, hi, ht)}H${w}` : '');
const ago = (ms) => { const s = Math.round(ms / 1000); return s < 60 ? s + ' s ago' : `${Math.floor(s / 60)} min ${s % 60} s ago`; };
const fx = (v, d = 1) => (v == null ? '–' : v.toFixed(d));
const dbm = (v) => (v == null ? '–' : '−' + Math.abs(Math.round(v)));
const kb = (v) => (v == null ? '–' : String(Math.round(v / 1024)));

// column k of H over the last `span` ms to `now`, in a w x ht box; fill: also the area under it
function trace(k, lo, hi, w, ht, now, span, fill) {
  let d = '', a = '', seg = [], prev = 0;
  const end = () => {
    if (seg.length) {
      d += 'M' + seg.join('L');
      if (fill) a += `M${seg[0][0]} ${ht}L${seg.join('L')}L${seg[seg.length - 1][0]} ${ht}Z`;
    }
    seg = [];
  };
  for (const r of H) {
    if (r[0] < now - span) continue;
    if (r[k] == null || r[0] - prev > 3 * POLL_MS) end();
    prev = r[0];
    if (r[k] != null) seg.push([(w - (now - r[0]) / span * w).toFixed(1), yOf(r[k], lo, hi, ht)]);
  }
  end();
  return fill ? [d, a] : d;
}

function chart(w, ht, label, paths) {
  return `<svg viewBox="0 0 ${w} ${ht}" preserveAspectRatio="none" style="height:${ht}px" role="img" aria-label="${label}">` +
    paths.filter((p) => p[0]).map(([d, c]) => `<path class="${c}" d="${d}"/>`).join('') +
    `<path class="z" d="M0 ${ht - 0.5}H${w}"/></svg>`;
}

const ICON = {
  lim: '<svg width="12" height="12" viewBox="0 0 12 12" aria-hidden="true"><circle cx="6" cy="6" r="4.6" fill="none" stroke="#e8a33d" stroke-width="1.4"/><path d="M6 1.4a4.6 4.6 0 0 1 0 9.2z" fill="#e8a33d"/></svg>',
  bad: '<svg width="12" height="12" viewBox="0 0 12 12" aria-hidden="true"><path d="M6 1L11.2 10.5H.8z" fill="#e5534b"/></svg>',
};

// Play: out and in now, the last 5 minutes, the charger and the budget.
function powerCard() {
  const p = S.power, g = S.governor, c = p.contract || {};
  if (p.state !== 'on') return null;  // powerNotice says why the speakers are off
  const now = Date.now(), last = H[H.length - 1], bud = g.budget_w;
  const sag = online && c.v && p.vbus_v != null && p.vbus_v < 0.9 * c.v;
  const lim = online && g.limiting && g.gain_db < -0.5 && g.reason !== 'heat' && g.reason !== 'amp_hot';
  const msg = lim ? ['lim', 'Limiting.', `Volume turned down ${(-g.gain_db).toFixed(1)} dB to stay within the ${Math.round(c.w)} W charger.`]
    : sag ? ['bad', 'Supply sagging.', `${p.vbus_v.toFixed(1)} V from a ${c.v} V charger. Use a stronger charger or turn it down.`] : null;
  let peak = 0;
  for (const r of H) if (r[0] >= now - PLAY_SPAN) peak = Math.max(peak, r[1] || 0);
  const budOn = bud > 0 && peak > 0.35 * bud, top = niceTop(Math.max(peak * 1.15, budOn ? bud * 1.08 : 1));
  const [o, a] = trace(2, 0, top, 330, 64, now, PLAY_SPAN, true);
  const v = (x) => (online ? fx(x) : '–'), off = online ? null : '';
  const label = `Out and in power, last 5 minutes. Out ${v(g.out_w)} W, in ${v(g.in_w)} W` + (bud ? `, budget ${bud} W.` : '.');
  return h('section.card.gr.pw', { 'aria-label': 'Power' },
    h('div.pw-h', {}, h('h2', {}, 'Power'), c.v && h('span.pw-ct', {}, `${c.v} V · ${Math.round(c.w)} W charger`)),
    msg && h('p.pw-msg.' + msg[0], { role: 'status', innerHTML: ICON[msg[0]] }, h('span', {}, h('b', {}, msg[1]), ' ' + msg[2])),
    h('div.pw-n', { 'data-off': off },
      h('div', {}, h('div.k', {}, h('i.sw'), 'Out'), h('div.v', {}, v(g.out_w), h('small', {}, 'W'))),
      h('div', {}, h('div.k', {}, h('i.sw.in'), 'In'), h('div.v', {}, v(g.in_w), h('small', {}, 'W'))),
      h('div.b', {}, sag ? 'Supply' : 'Budget', h('b', {}, sag ? fx(p.vbus_v) + ' V' : fx(bud) + ' W'))),
    h('div.pw-g', { 'data-off': off,
      innerHTML: chart(330, 64, label, [[a, 'a'], [budOn && level(bud, 0, top, 330, 64), 'b'],
        [trace(1, 0, top, 330, 64, now, PLAY_SPAN), 'i'], [o, 'o']]) },
    budOn && h('span.bl', { style: `top:${yOf(bud, 0, top, 64)}px` }, 'budget'),
    online && H.length < 15 && h('span.wait', {},
      `Graph fills in · ${last ? Math.round((last[0] - H[0][0]) / 1000) : 0} s so far`)),
    h('div.ax', {}, h('span', {}, '5 min ago'), h('span', {}, online || !last ? 'now' : 'last seen ' + ago(now - last[0]))));
}

// Setup: power, supply, Wi-Fi and memory over the last 15 minutes, with one
// crosshair: hover or touch a graph to read every value at that moment.
let pickF = null;  // where on the graphs the pointer is (0 to 1 across), or null

function diagGraphs() {
  if (!H.length) return null;
  const now = Date.now(), sp = DIAG_SPAN, W = 358, L = H[H.length - 1], c = S.power.contract || {};
  const bud = S.governor.budget_w, n = (t) => (online ? t : '–');
  let pk = 0, hk = 0, vlo = 99, vhi = 0, lim = '';
  for (const r of H) {
    if (r[0] < now - sp) continue;
    pk = Math.max(pk, r[1] || 0); hk = Math.max(hk, r[6] || 0);
    if (r[4] != null) { vlo = Math.min(vlo, r[4]); vhi = Math.max(vhi, r[4]); }
    if (r[7]) lim += `M${(W - (now - r[0]) / sp * W).toFixed(1)} 94.5h${(POLL_MS / sp * W).toFixed(1)}`;
  }
  const top = niceTop(Math.max(pk * 1.15, bud > 0 && pk > 0.35 * bud ? bud * 1.08 : 1));
  const [o, a] = trace(2, 0, top, W, 96, now, sp, true);
  const sagV = c.v ? 0.9 * c.v : null;
  const [lo, hi] = c.v ? [0.85 * c.v, 1.03 * c.v] : [vlo - 0.5, vhi + 0.5];
  const hTop = Math.max(100, Math.ceil(hk / 1024 / 50) * 50) * 1024;
  const ch = (name, unit, now_, bad, ht, label, paths, legend) => h('div.c', {},
    h('div.c-h', {}, h('span', {}, h('b', {}, name), ' · ' + unit), h('span.now' + (bad ? '.bad' : ''), {}, now_)),
    legend, h('div', { innerHTML: chart(W, ht, label, paths) }));
  const cs = h('div.cs', {
    onpointerdown: pickAt, onpointermove: pickAt,
    onpointerleave: (e) => { if (e.pointerType === 'mouse') { pickF = null; showPick(e.currentTarget); } } },
  ch('Power', 'W', n(`out ${fx(L[2])} · in ${fx(L[1])} W`), false, 96,
    `Out and in power, last 15 minutes` + (bud ? `, with the ${bud} W budget` : ''),
    [[lim, 'l'], [a, 'a'], [level(bud, 0, top, W, 96), 'b'], [trace(1, 0, top, W, 96, now, sp), 'i'], [o, 'o']],
    h('div.lg', {}, h('span', {}, h('i.sw'), 'Out'), h('span', {}, h('i.sw.in'), 'In'),
      bud != null && h('span', {}, h('i.sw.bu'), `Budget ${fx(bud)} W`), lim && h('span', {}, h('i.sw.li'), 'limiting'))),
  ch('Supply', 'V', n(`${fx(L[4], 2)} V`) + (sagV ? ` · low below ${fx(sagV)} V` : ''), online && sagV && L[4] != null && L[4] < sagV, 48,
    'Supply voltage, last 15 minutes',
    [[level(c.v, lo, hi, W, 48), 'r'], [level(sagV, lo, hi, W, 48), 't'], [trace(4, lo, hi, W, 48, now, sp), 'v']]),
  H.some((r) => r[5] != null) && ch('Wi-Fi signal', 'dBm', n(`${dbm(L[5])} dBm`) + ' · weak below −75 dBm', online && L[5] != null && L[5] < -75, 48,
    'Wi-Fi signal, last 15 minutes', [[level(-75, -90, -40, W, 48), 't'], [trace(5, -90, -40, W, 48, now, sp), 'v']]),
  ch('Free memory', 'KB', n(`${kb(L[6])} KB`) + ' · low below 16 KB', online && L[6] != null && L[6] < 16384, 48,
    'Free memory, last 15 minutes', [[level(16384, 0, hTop, W, 48), 't'], [trace(6, 0, hTop, W, 48, now, sp), 'v']]),
  h('div.ax', {}, h('span', {}, '15 min ago'), h('span', {}, '10'), h('span', {}, '5'), h('span', {}, 'now')),
  h('div.xh', { hidden: true }));
  const gs = h('div.gs.gr', {},
    h('div.gs-h', {}, h('h3', {}, 'Last 15 minutes'), h('span.dim', {}, online ? 'kept while this page is open' : 'last seen ' + ago(now - L[0]))),
    h('div.rd'), cs);
  showPick(cs, now);
  return gs;
}

function pickAt(e) {
  const r = e.currentTarget.getBoundingClientRect();
  pickF = Math.max(0, Math.min(1, (e.clientX - r.left) / r.width));
  showPick(e.currentTarget);
}

// the readout and the crosshair, for pickF, without redrawing the page
function showPick(cs, now = Date.now()) {
  let r = null;
  const pickT = pickF == null ? null : now - (1 - pickF) * DIAG_SPAN;
  if (pickT != null && pickT >= now - DIAG_SPAN) for (const s of H) if (!r || Math.abs(s[0] - pickT) < Math.abs(r[0] - pickT)) r = s;
  if (r && Math.abs(r[0] - pickT) > 2 * POLL_MS) r = null;
  const xh = $('.xh', cs);
  xh.hidden = !r;
  if (r) xh.style.left = (100 - (now - r[0]) / DIAG_SPAN * 100).toFixed(2) + '%';
  cs.previousSibling.replaceChildren(...(r ? [h('b', {}, now - r[0] < POLL_MS ? 'now' : ago(now - r[0])),
    ' · out ', h('b', {}, fx(r[2]) + ' W'), ' · in ', h('b', {}, fx(r[1]) + ' W'), h('br'),
    `${fx(r[4], 2)} V · ${dbm(r[5])} dBm · ${kb(r[6])} KB free`]
    : [pickT == null ? 'Touch a graph to read the values at that moment.' : 'No reading at that moment.']));
}

// The calls go out together: each is a round trip, and through the relay one
// costs about 120 ms, so four in a row held the first draw for half a second.
async function poll(boot) {
  if (uploading) { setTimeout(() => poll(boot), POLL_MS); return; }
  try {
    const w = POLL_WAIT_MS;
    const party = P !== null && tab !== 'sound'
      ? api('party', undefined, w).catch((e) => (e.status === 404 ? null : P)) : P;
    const others = tab === 'speakers' && I ? pollSpeakers() : null;
    // info too while there is none or the board was out of reach
    const fresh = !I || !online;
    const [i, st, p] = await Promise.all([fresh ? api('info', undefined, w) : I, api('state', undefined, w), party]);
    if (fresh) I = i;
    // and again after a restart (an update changes the firmware version)
    else if (S && st.uptime_s < S.uptime_s) I = await api('info', undefined, w);
    P = p;
    await others;
    if (boot && st.net && st.net.mode === 'ap' && !PAGES[want]) tab = 'setup';
    if (st && st.audio) { S = st; remember(st); }
    online = true; slow = 0;
  } catch (e) {
    // one poll out of time is a busy board; a second in a row, or any other failure, is a lost one
    online = online && e.name === 'AbortError' && ++slow < 2;
  }
  // one draw per poll; a bug in a page isn't the receiver going away, and
  // whatever went wrong, keep polling
  try { draw(); } finally { $('#conn').classList.toggle('on', online); setTimeout(() => poll(boot && !online), POLL_MS); }
}

function draw() {
  const typing = document.activeElement && /INPUT|SELECT|TEXTAREA/.test(document.activeElement.tagName) &&
    document.activeElement.type !== 'range';
  if (holding || typing) return;
  if (I) $('#name').textContent = tab === 'speakers' ? 'Speakers' : I.name;
  for (const b of document.querySelectorAll('#tabs button')) b.classList.toggle('on', b.dataset.tab === tab);
  const view = $('#view');
  const y = window.scrollY;
  // lost after it was found: say so above the last state rather than show it as live
  const page = S && I ? [!online && offline(), PAGES[tab]()] : [offline()];
  view.replaceChildren(...page.flat().filter((k) => k != null && k !== false));
  window.scrollTo(0, y);
}

// the join this page asked for, while it is recent enough to mention
function recentJoin() {
  if (joining && Date.now() - joining.at > JOIN_KEEP_MS) joining = null;
  return joining;
}

function offline() {
  const joining = recentJoin();
  return h('div.card.err', {}, h('div.big', {}, 'Looking for the receiver…'),
    h('p.dim', {}, joining ? `If it joined ${joining.ssid}, reconnect this phone to ${joining.ssid} and open ` +
      `http://${(S && S.net.hostname) || 'hifi'}.local. If not, its own HiFi-… network comes back: join that and try again.`
      : 'Check that it is powered and that this phone is on the same Wi-Fi. ' +
      'If it is new, join its own Wi-Fi network (named HiFi-…) and this page opens by itself.'));
}

// ---- Speakers -----------------------------------------------------------
// Every receiver on this Wi-Fi (the design: ~/designs/hifi-lite-speakers-af9de,
// docs/app.md "Speakers"): GET peers from this one (it asks mDNS for the
// others), then each other one's state and party at its own address. A card
// opens that receiver's own Play, Sound and Setup: this one's here, another's
// at its own address, which serves the same app.

const PEERS_MS = 10000; // the list is read again this often (the firmware asks mDNS at most every 10 s)
const PARTY_MAX = 3;    // followers a leader takes (hifi_party_group.h)
let PEERS = null;       // GET peers' rows, this one first; null before the first read, false on a build without it
let peersAt = 0;
const R = {};           // ip -> {st, party, ok, fails, err}: what the other receivers last said
// the party sheet: null (shut), 'off', 'lead', 'join' (starting one) or 'on'
let sheet = null, pickLead = null, pickJoin = new Set(), partyErr = '';
const at = (p) => (p.self ? '' : `http://${p.ip}/`);

async function pollSpeakers() {
  const w = POLL_WAIT_MS;
  if (PEERS !== false && Date.now() - peersAt > PEERS_MS) {
    peersAt = Date.now();
    const r = await api('peers', undefined, w).catch((e) => (e.status === 404 ? false : null));
    if (r === false) PEERS = false;
    else if (r && r.peers) PEERS = r.peers;
  }
  await Promise.all((PEERS || []).filter((p) => !p.self).map(async (p) => {
    const r = R[p.ip] || (R[p.ip] = { fails: 0 });
    try {
      const [st, party] = await Promise.all([api('state', undefined, w, at(p)),
        api('party', undefined, w, at(p)).catch((e) => (e.status === 404 ? null : r.party))]);
      Object.assign(r, { st, party, ok: true, fails: 0 });
    } catch (e) {
      // as for this one: a second miss in a row is a lost one
      r.ok = r.ok && ++r.fails < 2;
    }
  }));
}

// one row per receiver, the ones that answer first: {name, ip, model, self, st, party, ok, seen_s, err}
function speakers() {
  const self = { name: I.name, ip: S.net.ip, model: I.model, self: true };
  const all = (PEERS || [self]).map((p) => (p.self ? { ...p, name: I.name, st: S, party: P, ok: online }
    : { ...p, ...R[p.ip], name: (R[p.ip] && R[p.ip].ok && R[p.ip].st.name) || p.name }));
  return all.filter((p) => p.ok).concat(all.filter((p) => !p.ok));
}

// a change on one receiver: this one's state through take(), another's into R.
// An error stays on that card (or the party sheet) with what it said.
async function to(p, path, body) {
  try {
    const r = await api(path, body, POLL_WAIT_MS, at(p));
    const x = p.self ? null : R[p.ip];
    if (x) x.err = '';
    if (path === 'party') { if (x) x.party = r; else P = r; } else if (x) x.st = r; else take(r);
    draw();
    return r;
  } catch (e) {
    const why = e.status ? `It said: “${e.message}”.` : 'It may be off or out of Wi-Fi range.';
    if (path === 'party') partyErr = `${p.name} didn’t take that. ${why}`;
    else if (p.self) toast(e.message);
    else R[p.ip].err = `${p.name} didn’t answer. ${why}`;
    draw();
    return null;
  }
}

const leaderOf = (all) => all.find((p) => p.ok && p.party && p.party.on && p.party.role === 'leader');
const inParty = (all) => all.filter((p) => p.ok && p.party && p.party.on);

function pageSpeakers() {
  const all = speakers(), L = leaderOf(all);
  return [
    powerNotice(), // this box's own power trouble shows on the first page too
    h('div.grid', {}, all.map((p) => speakerCard(p, L))),
    PEERS === false ? h('p.dim', {}, 'This receiver’s firmware lists only itself: update it to see the others here.')
      : all.length < 2 && (PEERS ? h('div.card', {}, h('b', {}, 'Only this speaker found'),
        h('p.dim', {}, 'Others show here when they are on the same Wi-Fi. Check that they are plugged in and set up.'))
        : h('div.look', {}, h('span.spin', {}), 'Looking for other speakers on this Wi-Fi…')),
    partyDock(all, L),
    ...(sheet ? partySheet(all, L) : []),
  ];
}

const MODELS = { 'hifi-lite': 'Lite', 'hifi-plus': 'Plus', 'hifi-ultra': 'Ultra', lite: 'Lite', plus: 'Plus', ultra: 'Ultra' };
const mmss = (n) => `${Math.floor(n / 60)}:${String(n % 60).padStart(2, '0')}`;

// [dim lead-in, title, " — artist"] for the card's now-playing line
function nowPlaying(p, L) {
  const a = p.st.audio, np = a.playing || {};
  if (p.party && p.party.role === 'follower' && L && L !== p) {
    const n = L.st.audio.playing || {};
    return n.title ? ['', n.title, n.artist ? ` — ${n.artist}` : ''] : [`Waiting for ${L.name} to play`, '', ''];
  }
  if (a.input && a.input !== 'stream') return [INPUTS[a.input] || a.input, '', ''];
  if (np.state === 'play' || np.state === 'pause') {
    return [`${np.state === 'pause' ? 'Paused' : SOURCES[a.source] || a.source}${np.title ? ' · ' : ''}`,
      np.title || '', np.artist ? ` — ${np.artist}` : ''];
  }
  return [p.party && p.party.role === 'leader' ? `Waiting: play from your phone to ${p.name}` : 'Idle', '', ''];
}

function speakerCard(p, L) {
  const open = () => {
    if (!p.self) { location.href = at(p) + '#play'; return; }
    tab = 'play'; draw(); window.scrollTo(0, 0);
  };
  const head = h('button.sp-h', { onclick: open, 'aria-label': `Open ${p.name}` },
    h('span.dot' + (p.ok ? '.on' : ''), {}), h('span.sp-n', {}, p.name),
    h('span.chip', {}, MODELS[p.model] || p.model || ''), p.self && h('span.self', {}, 'this one'), h('span.chev', {}, '›'));
  if (!p.ok || !p.st || !p.st.audio) {
    return h('div.card.sp.off', { 'data-ip': p.ip }, head,
      h('p.note', {}, `Offline${p.seen_s ? ` · last heard ${ago(p.seen_s * 1000)}` : ''}`));
  }
  const a = p.st.audio, bt = p.st.bt, r = p.party || {};
  const follows = r.on && r.role === 'follower';
  const pills = [
    r.on && r.role === 'leader' && h('span.pill.p', {}, 'Leads party'),
    follows && h('span.pill.p', {}, L ? `Follows ${L.name}` : 'In the party'),
    r.on && r.role === 'listening' && h('span.pill', {}, 'Looking for the party'),
    bt && bt.pairing && h('span.pill.p', {}, `Pairing · ${mmss(bt.pairing_left_s)}`),
    bt && !bt.pairing && bt.connected && h('span.pill', {}, 'Phone connected'),
  ].filter(Boolean);
  const np = a.playing || {}, [lead, title, artist] = nowPlaying(p, L);
  const ctl = (action) => to(p, 'control', { action });
  const num = h('span.n' + (a.mute ? '.muted' : ''), {}, String(a.volume));
  return h('div.card.sp', { 'data-ip': p.ip }, head,
    pills.length ? h('div.pills', {}, pills) : null,
    h('div.np', {}, h('span.t', {}, lead, title && h('b', {}, title), artist),
      !follows && (np.state === 'play' || np.state === 'pause') && [
        h('button.sm', { 'aria-label': `${np.state === 'play' ? 'Pause' : 'Play'} ${p.name}`,
          onclick: () => ctl(np.state === 'play' ? 'pause' : 'play') }, np.state === 'play' ? '⏸' : '▶'),
        h('button.sm', { 'aria-label': `Next on ${p.name}`, onclick: () => ctl('next') }, '⏭')]),
    h('div.vol', {},
      h('button.sm', { 'aria-label': `${a.mute ? 'Unmute' : 'Mute'} ${p.name}`, onclick: () => to(p, 'volume', { mute: !a.mute }) },
        a.mute ? '🔇' : '🔊'),
      h('div.grow', {}, slider({ min: 0, max: a.max_volume, value: a.volume, 'aria-label': `Volume of ${p.name}`,
        show: (v) => { num.textContent = v; } }, (v) => to(p, 'volume', { volume: v }))),
      num),
    p.err && h('div.pw-msg.bad', {}, h('span', {}, p.err), h('button', { onclick: () => { R[p.ip].err = ''; draw(); } }, 'OK')));
}

// the bar at the bottom: what the party is doing, and the way into it
function partyDock(all, L) {
  const can = all.filter((p) => p.ok && p.party && p.party.key);
  const n = inParty(all).length;
  const label = n ? (L ? `Party · ${L.name} leads${n > 1 ? `, ${n - 1} follow${n > 2 ? '' : 's'}` : ''}` : 'Party · looking for a leader')
    : can.length < 2 ? 'Party needs a second speaker' : 'Start a party';
  return h('div.dock', {}, h('button' + (n ? '.party' : ''), {
    disabled: !n && can.length < 2,
    onclick: () => { sheet = n ? 'on' : 'off'; partyErr = ''; draw(); },
  }, '♫ ', label));
}

async function partyOp(p, op) {
  partyErr = '';
  const r = await to(p, 'party', { op });
  if (r) setTimeout(poll, 300);
  return r;
}

function partySheet(all, L) {
  const can = all.filter((p) => p.ok && p.party && p.party.key);
  const inIt = inParty(all), fol = inIt.filter((p) => p !== L);
  if (sheet === 'on' && !inIt.length) sheet = 'off';
  const shut = () => { sheet = null; draw(); };
  const body = [];
  if (sheet === 'off') {
    body.push(h('p.lede', {}, `Play the same music on several speakers, in step, over Wi-Fi. One speaker leads; up to ${PARTY_MAX} follow it.`),
      h('p.lede.note', {}, 'Each one keeps its own volume. Followers turn their Bluetooth off.'),
      h('div.acts', {}, h('button.pri', { onclick: () => {
        const playing = can.find((p) => (p.st.audio.playing || {}).state === 'play');
        pickLead = (playing || can.find((p) => p.self) || can[0]).ip; pickJoin = new Set(); sheet = 'lead'; draw();
      } }, 'Start a party')));
  } else if (sheet === 'lead') {
    body.push(h('h2', {}, 'Step 1 of 2 · Who leads?'),
      h('p.lede.note', {}, 'Pick the speaker your phone plays to. It keeps its Bluetooth and sends the music to the others.'),
      h('ul.pick', {}, can.map((p) => h('li', {}, h('label', {},
        h('input', { type: 'radio', name: 'lead', checked: p.ip === pickLead, onchange: () => { pickLead = p.ip; } }),
        h('span.x', {}, h('b', {}, p.name), h('span.s', {}, nowPlaying(p, null).join(''))))))),
      h('div.acts', {}, h('button', { onclick: () => { sheet = 'off'; draw(); } }, 'Back'),
        h('button.pri', { onclick: () => { sheet = 'join'; draw(); } }, 'Next')));
  } else if (sheet === 'join') {
    const lead = can.find((p) => p.ip === pickLead) || can[0];
    const rest = can.filter((p) => p !== lead);
    body.push(h('h2', {}, `Step 2 of 2 · Who joins ${lead.name}?`),
      h('p.lede.note', {}, `Tick up to ${PARTY_MAX}. A phone on a follower’s Bluetooth disconnects.`),
      h('ul.pick', {}, rest.map((p) => {
        const no = !pickJoin.has(p.ip) && pickJoin.size >= PARTY_MAX;
        return h('li', {}, h('label' + (no ? '.no' : ''), {},
          h('input', { type: 'checkbox', checked: pickJoin.has(p.ip), disabled: no,
            onchange: (e) => { if (e.target.checked) pickJoin.add(p.ip); else pickJoin.delete(p.ip); draw(); } }),
          h('span.x', {}, h('b', {}, p.name), p.st.bt && p.st.bt.connected && h('span.s', {}, 'Its phone disconnects'))));
      })),
      h('div.acts', {}, h('button', { onclick: () => { sheet = 'lead'; draw(); } }, 'Back'),
        h('button.pri', { disabled: !pickJoin.size, onclick: async () => {
          // the first one switched on leads; the others then join it by themselves
          if (!await partyOp(lead, 'on')) return;
          for (const p of rest) if (pickJoin.has(p.ip)) await partyOp(p, 'on');
          sheet = 'on'; draw();
        } }, 'Start')));
  } else {
    const out = can.filter((p) => !p.party.on);
    const full = L && (L.party.followers || []).length >= PARTY_MAX;
    const status = (p) => (p.party.why ? h('span.st.w', {}, h('i'), `Not in step: ${p.party.why}`)
      : p.party.role === 'follower' ? h('span.st.k', {}, h('i'), 'In step') : h('span.st.j', {}, h('i'), 'Joining…'));
    body.push(h('h2', {}, 'Leads'),
      h('div.card', {}, L ? h('b', {}, L.name) : h('span.dim', {}, 'Looking for a leader…')),
      h('h2', {}, 'Following'),
      h('div.card', {}, fol.length ? h('ul.pick.fl', {}, fol.map((p) => h('li', {}, h('div.row', {},
        h('span.x', {}, h('b', {}, p.name), status(p))),
        h('div.acts.s', {}, h('button', { onclick: () => partyOp(p, 'lead') }, 'Lead from this one'),
          h('button', { onclick: () => partyOp(p, 'off') }, 'Remove')))))
        : h('p.dim', {}, 'No one follows yet. Add a speaker below.')),
      h('h2', {}, 'Add a speaker'),
      h('div.card', {}, full ? h('p.dim', {}, `Full: a party takes up to ${PARTY_MAX} followers. Remove one to add another.`)
        : out.length ? h('ul.pick.fl', {}, out.map((p) => h('li', {}, h('div.row', {}, h('b.grow', {}, p.name),
          h('button', { onclick: () => partyOp(p, 'on') }, 'Add')))))
          : h('p.dim', {}, 'Every speaker that’s on is in the party.')),
      h('button.stop', { onclick: async () => {
        // followers first: a leader that leaves first hands the lead on
        for (const p of [...fol, ...inIt.filter((x) => x === L)]) await partyOp(p, 'off');
        sheet = null; draw();
      } }, 'Stop party'),
      h('p.note', {}, 'Every speaker goes back to playing on its own.'));
  }
  return [h('button.scrim', { 'aria-label': 'Close', onclick: shut }),
    h('div.sheet', { role: 'dialog', 'aria-label': 'Party' }, h('div.grab'),
      h('div.sh-h', {}, h('h1', {}, 'Party'), h('button', { onclick: shut }, 'Done')),
      partyErr && h('div.pw-msg.bad', {}, partyErr), body)];
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
  if (a.hot) return h('div.card.warn', {}, h('b', {}, 'Amplifier hot. '),
    'It is turning itself down until it cools. A fan, or a lower Heat cap on the Sound page, keeps it playing.');
  if (g.limiting && g.gain_db < -0.5 && (g.reason === 'heat' || g.reason === 'amp_hot'))
    return h('div.card.warn', {}, h('b', {}, 'Heat limit. '),
      `Turned down ${(-g.gain_db).toFixed(1)} dB to keep the amplifier under ${g.heat_cap_w} W a channel. ` +
      'You can raise the Heat cap on the Sound page.');
  // a power limit is said in the Power card under Volume
  return null;
}

function pagePlay() {
  const a = S.audio, np = a.playing || {};
  const direct = a.input !== 'stream';
  // Bluetooth without track details still plays: show it, with its buttons
  const idle = direct || a.source === 'idle' || !(np.title || np.state === 'play' || np.state === 'pause');
  const title = direct ? INPUTS[a.input] : idle ? 'Nothing playing'
    : np.title || (np.state === 'pause' ? 'Paused' : 'Playing');
  const sub = direct ? DIRECT_NOTES[a.input] || ''
    : idle ? `Play to “${I.name}” from Spotify, AirPlay, Bluetooth or Lyrion.`
      : [np.artist, np.album].filter(Boolean).join(' — ');
  return [
    powerNotice(),
    h('div.card', {},
      h('h2', {}, direct ? 'Playing from an input' : idle ? 'Ready' : `Playing from ${SOURCES[a.source] || a.source}`),
      h('div.np-title', {}, title), h('div.np-sub', {}, sub),
      !idle && h('div.row', { style: 'justify-content:center;margin-top:14px' },
        h('button.icon', { 'aria-label': 'Previous', onclick: () => set('control', { action: 'prev' }) }, '⏮'),
        h('button.icon.pri', { 'aria-label': np.state === 'play' ? 'Pause' : 'Play',
          onclick: () => set('control', { action: np.state === 'play' ? 'pause' : 'play' }) }, np.state === 'play' ? '⏸' : '▶'),
        h('button.icon', { 'aria-label': 'Next', onclick: () => set('control', { action: 'next' }) }, '⏭'))),
    volumeCard(),
    powerCard(),
    h('div.card', {}, h('h2', {}, 'Input'),
      seg(Object.fromEntries(['stream', ...I.caps.line_modes].map((m) => [m, INPUTS[m] || m])), a.input,
        (m) => set('input', { input: m })),
      h('p.dim', {}, INPUT_NOTES[a.input] || '')),
    partyCard(),
  ];
}

// Party: play in step with the other receivers on this Wi-Fi (docs/app.md).
// The first one switched on leads unless one is picked; the others join it.
function partyCard() {
  if (!P) return null;
  const go = (op) => api('party', { op }).then((r) => { P = r; draw(); }, (e) => toast(e.message));
  const n = (P.followers || []).length;
  const now = !P.on ? null
    : P.role === 'leader' ? (n ? `Leading: ${n} more receiver${n > 1 ? 's play' : ' plays'} along.` : 'Leading. Switch party on on the others.')
      : P.role === 'follower' ? `Playing along with ${P.leader || P.leader_ip}.` : 'Looking for the others…';
  return h('div.card', {}, h('h2', {}, 'Party'),
    h('p.dim', {}, 'Play in step with your other receivers on this Wi-Fi. Start the music on one, connecting your phone ' +
      'first, then switch party on there and on the others: they join it. Bluetooth is off on the ones that follow.'),
    P.key ? h('div.row', {}, h('span.grow', {}, 'Party'), onOff(P.on, (on) => go(on ? 'on' : 'off')))
      : h('p.dim', {}, 'This receiver has no party key yet.'),
    now && h('p', {}, now, P.why ? h('span.dim', {}, ` (${P.why})`) : ''),
    P.on && P.role !== 'leader' && h('div.row', {}, h('button', { onclick: () => go('lead') }, 'Lead from this one')));
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
    c.channel && channelCard(),
    h('div.card', {}, h('h2', {}, 'Volume limit'),
      h('p.dim', {}, 'The highest the volume can go, from the app, buttons or remote. Turn it down to protect smaller speakers.'),
      h('div.row', {}, h('div.grow', {}, slider({ min: 30, max: 100, value: a.max_volume, 'aria-label': 'Volume limit',
        show: (v) => { $('#maxv').textContent = v; } }, (v) => set('volume', { max_volume: v }))),
      h('span.big', { id: 'maxv' }, String(a.max_volume)))),
    loudnessCard(),
  ];
}

// a labelled slider with its value beside it
function line(label, attrs, value, unit, send) {
  const v = h('span.dim', { style: 'min-width:64px;text-align:right' }, value + unit);
  return h('div.row', {}, h('span', { style: 'min-width:84px' }, label),
    h('div.grow', {}, slider({ ...attrs, value, show: (x) => { v.textContent = x + unit; } }, send)), v);
}

// Boost (digital gain past full scale, under a peak limiter) and the heat cap
function loudnessCard() {
  const L = I.caps.loudness, b = S.audio.boost, g = S.governor;
  if (!L || !b) return null;
  const hc = L.heat_cap_w;
  const rows = [
    h('p.dim', {}, 'Boost makes everything louder. A fast limiter keeps the loud peaks from clipping, ' +
      'so loud songs sound a little squeezed at high Boost.'),
    line('Boost', { min: 0, max: L.boost_max_db, step: 0.5 }, b.db, ' dB', (x) => set('loudness', { boost_db: x })),
  ];
  if (b.db > 0) rows.push(h('p.dim', {}, b.limiting
    ? `Limiting now: peaks ${(-b.limit_db).toFixed(1)} dB down.` : 'Not limiting now.'));
  if (g.heat_cap_w != null) {
    rows.push(line('Heat cap', { min: hc.min, max: hc.max, step: 0.5 }, g.heat_cap_w, ' W',
      (x) => set('loudness', { heat_cap_w: x })),
    h('p.dim', {}, `How much sustained power each channel may give. ${hc.default} W is the safe default. ` +
      `Above ${hc.default} W the amplifier may cut out when it gets hot, more so in a closed box; a fan helps.`));
    if (g.reason === 'heat' || g.reason === 'amp_hot' || S.amp.hot)
      rows.push(h('p', {}, h('b', {}, S.amp.hot || g.reason === 'amp_hot'
        ? 'The amplifier is hot and is turning itself down.'
        : `Held at the heat cap now (${g.heat_w} W a channel).`)));
  }
  return h('div.card', {}, h('h2', {}, 'Loudness'), rows);
}

// which side the speaker output plays (Lite); SUB OUT follows it
function channelCard() {
  const ch = S.audio.channel || 'stereo';
  return h('div.card', {}, h('h2', {}, 'Speaker output'),
    h('p.dim', {}, 'Stereo for a pair. For one speaker on its own, pick Mono, or Left or Right ' +
      'when two receivers each play one side.'),
    seg({ stereo: 'Stereo', left: 'Left', right: 'Right', mono: 'Mono' }, ch, (v) => set('channel', { channel: v })));
}

function subCard() {
  const s = S.audio.sub, c = I.caps.sub;
  const rows = [h('div.row', {}, h('span.grow', {}, 'Sub outputs'), onOff(s.on, (on) => set('sub', { on })))];
  if (s.on) {
    rows.push(line('Level', { min: -30, max: c.max_db ?? 6, step: 0.5 }, s.level_db, ' dB', (x) => set('sub', { level_db: x })));
    if ((c.max_db ?? 6) < 0) rows.push(h('p.dim', {}, `This model's sub outputs top out at ${c.max_db} dB.`));
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
    wifiCard(), nameCard(), remoteCard(), (I.caps.led || I.caps.backlight) && lightsCard(), updateCard(), diagCard(),
  ];
}

function wifiCard() {
  const n = S.net;
  const pw = h('input', { type: 'password', placeholder: 'Wi-Fi password', autocomplete: 'off' });
  const other = h('input', { type: 'text', placeholder: 'Network name', autocapitalize: 'off' });
  const join = async (ssid) => {
    if (!ssid) return toast('Choose a network');
    pw.blur(); other.blur(); // or the page stays as it was while the keyboard is up
    const was = n.mode === 'sta' ? n.ssid : null;
    if (await set('wifi', { ssid, password: pw.value })) { joining = { ssid, at: Date.now(), was }; toast(`Joining ${ssid}…`); draw(); }
  };
  const nets = scan || [];
  const j = n.join || joinNow(n);
  return h('div.card', {}, h('h2', {}, 'Wi-Fi'),
    n.mode === 'sta' && h('div.row', {}, h('span.grow', {}, h('b', {}, n.ssid), h('div.dim', {},
      `${n.ip} · signal ${bars(n.rssi)}` + (n.eth ? ' · Ethernet connected' : ''))),
      h('button', { onclick: () => confirm(`Forget ${n.ssid}? The receiver goes back to its own HiFi-… network.`) &&
        set('wifi/forget', {}).then((ok) => { if (ok) joining = null; }) }, 'Forget')),
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

// The firmware answers a join at once and says nothing more: the result
// shows in net (on the network asked for, or not yet when the wait is up).
function joinNow(n) {
  if (!recentJoin()) return null;
  const { ssid, at, was } = joining;
  // already on it: the firmware hasn't tried the new password yet, so wait a little
  const settled = was !== ssid || Date.now() - at > JOIN_SAME_MS;
  if (n.mode === 'sta' && n.ssid === ssid && n.ip && settled) return { ssid, state: 'ok', ip: n.ip };
  if (Date.now() - at > JOIN_WAIT_MS) return { ssid, state: 'failed', reason: 'check the password and that it is a 2.4 GHz network' };
  return { ssid, state: 'joining' };
}

function bars(rssi) {
  return rssi > -55 ? '▂▄▆█' : rssi > -67 ? '▂▄▆' : rssi > -78 ? '▂▄' : '▂';
}

// Name and Bluetooth (the design's Setup card): rename, and "Pair a new
// phone" (POST bt/pair: findable for 2 minutes). pairing is what this page
// last asked: null, 'on', then 'ok' (a phone connected) or 'out' (it ran out).
let renaming = false, pairing = null;
const PAIR_S = 120;

function nameCard() {
  const bt = S.bt, follows = P && P.on && P.role === 'follower';
  if (bt && bt.pairing && !pairing) pairing = 'on'; // started on the box (− and + held) or another phone
  if (pairing === 'on' && bt && !bt.pairing) pairing = bt.connected ? 'ok' : 'out';
  const pair = async (on) => {
    try {
      S.bt = (await api('bt/pair', on ? {} : { on: false })).bt; // it answers {bt: {...}}, as GET state/bt
      pairing = on ? 'on' : null;
    } catch (e) { toast(e.status === 404 ? "Pairing isn't on this speaker's firmware yet" : e.message); }
    draw();
  };
  let top;
  if (!renaming) {
    top = [h('p.dim', {}, 'What Spotify, AirPlay and Bluetooth show this speaker as.'),
      h('div.row', {}, h('span.big.grow', {}, I.name), h('button', { onclick: () => { renaming = true; draw(); } }, 'Rename'))];
  } else {
    const inp = h('input', { type: 'text', value: I.name, maxLength: 32, 'aria-label': 'New name' });
    top = [h('label.dim', {}, 'New name'), inp,
      h('div.acts', {}, h('button', { onclick: () => { renaming = false; draw(); } }, 'Cancel'),
        h('button.pri', { onclick: async () => {
          inp.blur();
          if (!await set('name', { name: inp.value.trim() })) return;
          renaming = false;
          I = await api('info').catch(() => I); draw(); toast('Renamed');
        } }, 'Save'))];
  }
  if (!bt) return h('div.card', {}, h('h2', {}, 'Name'), top);
  const off = follows || !bt.on;
  const box = pairing === 'on' && bt.pairing ? h('div.pairbox', { role: 'status' },
    h('div.row', {}, h('span.cd', {}, mmss(bt.pairing_left_s)), h('span.sub.grow', {}, 'left to pair'),
      h('button', { onclick: () => pair(false) }, 'Cancel')),
    h('div.bar', { style: 'margin:12px 0' }, h('i', { style: `width:${Math.round(bt.pairing_left_s / PAIR_S * 100)}%` })),
    h('p', { style: 'margin:0' }, 'Now pick ', h('b', {}, `“${I.name}”`), ' in your phone’s Bluetooth list.'))
    : pairing === 'ok' ? h('div.pairbox', { role: 'status' },
      h('div.row', {}, h('b.grow.ok', {}, '✓ Phone connected'), h('button', { onclick: () => { pairing = null; draw(); } }, 'Done')),
      h('p.note', {}, `Play from it and pick “${I.name}”.`))
      : pairing === 'out' ? h('div.pairbox', { role: 'status' },
        h('p', { style: 'margin:0 0 4px' }, h('b', {}, 'No phone paired in 2 minutes')),
        h('p.note', {}, 'Keep the phone close, with its Bluetooth settings open.'),
        h('div.acts', {}, h('button', { onclick: () => { pairing = null; draw(); } }, 'Close'),
          h('button.pri', { onclick: () => pair(true) }, 'Try again')))
        : [h('button', { style: 'width:100%;min-height:48px;margin-top:12px', disabled: off, onclick: () => pair(true) },
          'Pair a new phone'),
        h('p.note', {}, follows ? `Bluetooth is off while ${I.name} follows the party.`
          : !bt.on ? 'Bluetooth is off on this speaker.'
            : `${I.name} shows up on phones for 2 minutes.${bt.connected ? ' The phone on it now disconnects.' : ''}`)];
  return h('div.card', {}, h('h2', {}, 'Name and Bluetooth'), top,
    h('div.row', { style: 'margin-top:14px;padding-top:12px;border-top:1px solid var(--line)' },
      h('span.grow', {}, 'Phone on Bluetooth'),
      h('span', { className: bt.connected && !off ? 'ok' : 'dim' }, off ? 'Off' : bt.connected ? 'Connected' : 'None')),
    box);
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
    check && (check.error ? h('p.bad', {}, `Couldn't check for updates: ${check.error}. Try again, or install from a file.`)
      : h('p', { className: newer ? '' : 'dim' }, newer ? `Version ${check.latest} is available. ${check.notes || ''}` : 'Up to date.')),
    !busy && h('div.row', {},
      newer ? h('button.pri', { onclick: () => set('update', { url: check.url }) }, 'Install')
        : h('button', { onclick: async () => {
          toast('Checking…');
          try { check = await api('update/check'); } catch (e) { check = { error: e.message }; }
          draw();
        } }, check ? 'Check again' : 'Check for updates'),
      h('button', { onclick: () => file.click() }, 'From a file…'), file));
}

function upload(f) {
  if (!f) return;
  const x = new XMLHttpRequest();
  uploading = true;
  x.onloadend = () => { uploading = false; };
  x.open('POST', 'api/v1/update/upload');
  x.setRequestHeader('Content-Type', 'application/octet-stream');
  x.upload.onprogress = (e) => {
    if (e.lengthComputable && S) { S.update = { state: 'downloading', progress: Math.round(e.loaded * 100 / e.total) }; draw(); }
  };
  x.onload = () => { try { take(JSON.parse(x.responseText)); } catch { /* the next poll shows it */ } };
  x.onerror = () => toast('Upload failed');
  x.timeout = 120000;   // a board gone after the last byte would otherwise hold the polls until the browser gives up
  x.ontimeout = () => toast('Upload failed: no answer');
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
    diagGraphs(),
    selftest && h('ul.list', { style: 'margin-top:10px' }, selftest.checks.map((t) => h('li', {},
      h('span', { className: t.pass ? 'ok' : 'bad' }, t.pass ? '✓' : '✗'),
      h('span.grow', {}, CHECKS[t.name] || t.name), h('span.dim', {}, t.value ?? '')))),
    h('div.row', {}, h('button', { onclick: async () => {
      toast('Running self-test…');
      try { selftest = await api('selftest', {}); draw(); } catch (e) { toast(e.message); }
    } }, 'Run self-test'),
    h('button', { onclick: () => confirm('Restart the receiver?') && set('reboot', {}).then((ok) => ok && toast('Restarting…')) }, 'Restart'),
    h('button', { onclick: async () => {
      if (!confirm('Erase Wi-Fi, name, remote and sound settings and start again from setup?')) return;
      // the receiver only erases once someone unplugs it (a page on the network can't)
      try {
        const r = await api('reset', { factory: true });
        toast(r.armed ? `To finish, unplug the receiver and plug it back in within ${Math.round(r.within_s / 60)} minutes. ` +
          'Then join the HiFi-… Wi-Fi to set it up again.' : 'Reset. Join the HiFi-… Wi-Fi to set it up again.', 20000);
      } catch (e) { toast(e.message); }
    } }, 'Factory reset')));
}

// the LED test and the front-panel backlight, on models that have them
function lightsCard() {
  const c = I.caps, bl = (S.controls && S.controls.backlight) || 'auto';
  return h('div.card', {}, h('h2', {}, 'Lights'),
    c.backlight && h('div.row', {}, h('span.grow', {}, 'Backlight'),
      seg({ auto: 'Auto', on: 'On', off: 'Off' }, bl, (mode) => set('backlight', { mode }))),
    c.led && h('div.row', {}, h('span.grow', {}, 'Light every LED for a few seconds'),
      h('button', { onclick: () => api('led', { mode: 'test' }).then(() => toast('LEDs on'), (e) => toast(e.message)) }, 'Test')));
}

function uptime(s) {
  if (s == null) return '—';
  const d = Math.floor(s / 86400), hr = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  return (d ? d + 'd ' : '') + (d || hr ? hr + 'h ' : '') + m + 'm';
}

const PAGES = { speakers: pageSpeakers, play: pagePlay, sound: pageSound, setup: pageSetup };

for (const b of document.querySelectorAll('#tabs button')) {
  b.addEventListener('click', () => { tab = b.dataset.tab; pickF = null; draw(); window.scrollTo(0, 0); });
}
const want = location.hash.slice(1);
if (PAGES[want]) tab = want;
if (want === 'party') { tab = 'speakers'; sheet = 'on'; } // the party sheet, from a bookmark
poll(true);
if ('serviceWorker' in navigator && window.isSecureContext) navigator.serviceWorker.register('sw.js').catch(() => {});
