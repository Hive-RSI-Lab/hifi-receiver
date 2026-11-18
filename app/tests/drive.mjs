// Drives the app in headless Chrome for the tests that click (test_app.py,
// class Clicks): node drive.mjs CHROME URL STEPS_JSON
// STEPS: [{see: "text"}, {gone: "text"}, {click: "button text"}, {tick: "label text"},
//         {range: "aria-label", value: n}, {type: "aria-label", value: "s"},
//         {js: "expression"} (prints its value), {shot: "file.png", w?: 390, h?: 900}
//         (a screenshot of the window, for app/screenshots.py)]
// Each step waits up to 8 s for its text or button. Prints "ok", or the
// step that failed and the page's text, and exits 1.
// Uses the DevTools protocol over Node's own WebSocket (Node 22+).
import { spawn } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [chrome, url, stepsJson] = process.argv.slice(2);
const steps = JSON.parse(stepsJson);
const prof = mkdtempSync(join(process.env.TMPDIR || tmpdir(), 'drive-'));
const proc = spawn(chrome, ['--headless=new', '--no-sandbox', '--disable-gpu', '--no-proxy-server',
  `--user-data-dir=${prof}`, '--remote-debugging-address=127.0.0.1', '--remote-debugging-port=0',
  '--window-size=420,900', url], { stdio: 'ignore' });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function page() {
  for (let i = 0; i < 100; i++) {
    try {
      const port = readFileSync(join(prof, 'DevToolsActivePort'), 'utf8').split('\n')[0];
      const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      const p = list.find((t) => t.type === 'page' && t.url.startsWith(url.split('#')[0]));
      if (p) return p.webSocketDebuggerUrl;
    } catch { /* not up yet */ }
    await sleep(100);
  }
  throw new Error('Chrome did not start');
}

let ws, id = 0;
const wait = new Map();
function send(method, params = {}) {
  return new Promise((ok) => { wait.set(++id, ok); ws.send(JSON.stringify({ id, method, params })); });
}
async function run(js) {
  const r = await send('Runtime.evaluate', { expression: js, returnByValue: true, awaitPromise: true });
  return r.result && r.result.result && r.result.result.value;
}

// in the page: find a button by its text or aria-label (the last match, so a
// sheet's button wins over the page's under it), or an input by aria-label
// (an icon before the text, like the dock's ♫, is skipped)
const FIND = `(t) => [...document.querySelectorAll('button')].filter((b) => { if (b.disabled) return false;
  const x = b.textContent.replace(/^[^\\p{L}\\p{N}]+/u, '').trim();
  return (b.getAttribute('aria-label') || '') === t || x === t || x.startsWith(t); }).pop()`;
const act = {
  see: (s) => `document.body.textContent.includes(${JSON.stringify(s.see)})`,
  gone: (s) => `!document.body.textContent.includes(${JSON.stringify(s.gone)})`,
  click: (s) => `(() => { const b = (${FIND})(${JSON.stringify(s.click)}); if (b) b.click(); return !!b; })()`,
  range: (s) => `(() => { const i = document.querySelector('input[aria-label="' + ${JSON.stringify(s.range)} + '"]');
    if (!i) return false; i.value = ${JSON.stringify(String(s.value))};
    i.dispatchEvent(new Event('change', { bubbles: true })); return true; })()`,
  tick: (s) => `(() => { const l = [...document.querySelectorAll('label')].find((x) => x.innerText.includes(${JSON.stringify(s.tick)}));
    const i = l && l.querySelector('input'); if (!i || i.disabled) return false; i.click(); return true; })()`,
  type: (s) => `(() => { const i = document.querySelector('input[aria-label="' + ${JSON.stringify(s.type)} + '"]');
    if (!i) return false; i.value = ${JSON.stringify(String(s.value))}; return true; })()`,
};

let code = 1;
try {
  ws = new WebSocket(await page());
  await new Promise((ok, no) => { ws.onopen = ok; ws.onerror = no; });
  ws.onmessage = (m) => { const d = JSON.parse(m.data); if (wait.has(d.id)) { wait.get(d.id)(d); wait.delete(d.id); } };
  let n = 0;
  for (const s of steps) {
    n++;
    if ('js' in s) { console.log('js:', JSON.stringify(await run(s.js))); continue; }
    if ('shot' in s) {
      await send('Emulation.setDeviceMetricsOverride', { width: s.w || 390, height: s.h || 900, deviceScaleFactor: 1, mobile: false });
      await send('Emulation.setScrollbarsHidden', { hidden: true });
      await sleep(600); // a redraw and the sheet's slide-in
      const r = await send('Page.captureScreenshot', { format: 'png' });
      writeFileSync(s.shot, Buffer.from(r.result.data, 'base64'));
      continue;
    }
    const k = Object.keys(act).find((x) => x in s);
    let done = false;
    for (let t = 0; t < 80 && !done; t++) {
      done = await run(act[k](s));
      if (!done) await sleep(100);
    }
    if (!done) {
      console.log(`step ${n} failed: ${JSON.stringify(s)}\n--- page ---\n${await run('document.body.innerText')}`);
      throw new Error('step');
    }
  }
  console.log('ok');
  code = 0;
} catch (e) {
  if (e.message !== 'step') console.log(String(e));
} finally {
  if (ws) ws.close();
  proc.kill('SIGKILL');
  await sleep(200);
  rmSync(prof, { recursive: true, force: true });
  process.exit(code);
}
