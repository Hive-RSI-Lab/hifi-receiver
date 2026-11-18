"""A small Chrome driver for the app's user-path tests: headless Chrome over
its DevTools pipe (--remote-debugging-pipe, file descriptors 3 and 4), so no
debugging port is opened. Standard library only.

    with Page(url) as p:
        p.wait("document.querySelector('#view h2')")
        p.click("Join")
        print(p.text())
"""
from __future__ import annotations

import json
import os
import select
import shutil
import subprocess
import tempfile
import time

CHROME = next((shutil.which(c) for c in ("google-chrome", "chromium", "chromium-browser") if shutil.which(c)), None)

# Helpers put into the page: find a control by what a person reads on it.
HELPERS = r"""
window.__t = {
  vis: (e) => !!(e && e.offsetParent !== null),
  find: (text, sel) => [...document.querySelectorAll(sel || 'button')].find((e) =>
    __t.vis(e) && ((e.textContent || '').trim() === text || e.getAttribute('aria-label') === text)),
  card: (title) => [...document.querySelectorAll('#view .card')].find((c) => {
    const h = c.querySelector('h2'); return h && h.textContent.trim() === title; }),
};
"""


class Error(Exception):
    pass


class Page:
    """One headless Chrome with one page open on `url`. confirm() always
    answers OK; every fetch the page makes is recorded in window.__calls."""

    def __init__(self, url: str, width: int = 390, height: int = 2400, timeout: float = 30):
        if not CHROME:
            raise Error("no Chrome on this machine")
        self.timeout, self._id, self._buf = timeout, 0, b""
        self._prof = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        to_chrome_r, self._w = os.pipe()
        self._r, from_chrome_w = os.pipe()

        def fds():  # in the child: Chrome reads commands on 3, writes replies on 4
            os.dup2(to_chrome_r, 3)
            os.dup2(from_chrome_w, 4)

        self.proc = subprocess.Popen(
            [CHROME, "--headless=new", "--no-sandbox", "--disable-gpu", "--no-proxy-server", "--no-first-run",
             "--remote-debugging-pipe", f"--user-data-dir={self._prof.name}", f"--window-size={width},{height}",
             "about:blank"],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            # the child closes every other descriptor after fds() runs: keep 3 and 4
            preexec_fn=fds, pass_fds=(3, 4))
        os.close(to_chrome_r)
        os.close(from_chrome_w)
        try:
            targets = self._send("Target.getTargets")["targetInfos"]
            tid = next(t["targetId"] for t in targets if t["type"] == "page")
            self._sid = self._send("Target.attachToTarget", targetId=tid, flatten=True)["sessionId"]
            self.cmd("Page.enable")
            self.cmd("Page.addScriptToEvaluateOnNewDocument", source=HELPERS + r"""
              window.confirm = () => true;
              window.__calls = [];
              const f = window.fetch;
              window.fetch = (u, o) => { window.__calls.push([(o && o.method) || 'GET', String(u),
                o && o.body ? JSON.parse(o.body) : null]); return f(u, o); };
            """)
            self.go(url)
        except Exception:
            self.close()
            raise

    # ---- the pipe

    def _send(self, method: str, session: str | None = None, **params) -> dict:
        self._id += 1
        msg = {"id": self._id, "method": method, "params": params}
        if session:
            msg["sessionId"] = session
        os.write(self._w, json.dumps(msg).encode() + b"\0")
        end = time.time() + self.timeout
        while True:
            while b"\0" not in self._buf:
                left = end - time.time()
                if left <= 0 or not select.select([self._r], [], [], left)[0]:
                    raise Error(f"Chrome didn't answer {method}")
                chunk = os.read(self._r, 1 << 16)
                if not chunk:
                    raise Error("Chrome closed the pipe")
                self._buf += chunk
            raw, self._buf = self._buf.split(b"\0", 1)
            r = json.loads(raw)
            if r.get("id") == self._id:
                if "error" in r:
                    raise Error(f"{method}: {r['error'].get('message')}")
                return r.get("result", {})

    def cmd(self, method: str, **params) -> dict:
        return self._send(method, self._sid, **params)

    # ---- the page

    def go(self, url: str):
        self.cmd("Page.navigate", url=url)
        self.wait("document.readyState === 'complete' && window.__t")

    def js(self, expr: str):
        """The value of a JS expression (awaited if it is a promise)."""
        r = self.cmd("Runtime.evaluate", expression=expr, awaitPromise=True, returnByValue=True)
        if "exceptionDetails" in r:
            d = r["exceptionDetails"]
            raise Error(f"{expr[:80]}: {d.get('exception', {}).get('description') or d.get('text')}")
        return r["result"].get("value")

    def wait(self, expr: str, timeout: float | None = None, what: str = ""):
        """Wait until a JS expression is truthy; return its value."""
        end = time.time() + (timeout or self.timeout)
        while True:
            # an element comes back as {} (falsy here): count it as found
            v = self.js(f"(() => {{ try {{ const v = {expr}; return v instanceof Node ? true : v; }} catch (e) {{ return null; }} }})()")
            if v:
                return v
            if time.time() > end:
                raise Error(f"timed out waiting for {what or expr}; the page reads: {self.text()[:600]}")
            time.sleep(0.1)

    def text(self) -> str:
        return self.js("document.body ? document.body.innerText : ''") or ""

    def toast(self) -> str:
        return self.js("(() => { const t = document.querySelector('#toast'); return t && !t.hidden ? t.textContent : ''; })()")

    def has(self, text: str, timeout: float | None = None) -> bool:
        """Wait for text on the page (as written, or as shown: headings are
        upper case on screen); True if it came."""
        t = json.dumps(text)
        try:
            self.wait(f"document.body.innerText.includes({t}) || document.body.textContent.includes({t})", timeout)
            return True
        except Error:
            return False

    def click(self, label: str, sel: str = "button", within: str | None = None):
        """Click the visible control whose text or aria-label is `label`
        (inside the card titled `within`, if given)."""
        scope = f"__t.card({json.dumps(within)})" if within else "document"
        self.wait(f"(() => {{ const s = {scope}; return s && [...s.querySelectorAll({json.dumps(sel)})].some((e) => "
                  f"__t.vis(e) && ((e.textContent || '').trim() === {json.dumps(label)} || e.getAttribute('aria-label') === {json.dumps(label)})); }})()",
                  what=f"a {sel} {label!r}" + (f" in {within!r}" if within else ""))
        self.js(f"(() => {{ const s = {scope}; [...s.querySelectorAll({json.dumps(sel)})].find((e) => "
                f"__t.vis(e) && ((e.textContent || '').trim() === {json.dumps(label)} || e.getAttribute('aria-label') === {json.dumps(label)})).click(); }})()")

    def tab(self, name: str):
        self.click(name, "#tabs button")

    def type(self, sel: str, text: str, within: str | None = None):
        """Focus an input, set its value as typing would, then leave it."""
        scope = f"__t.card({json.dumps(within)})" if within else "document"
        self.js(f"(() => {{ const e = {scope}.querySelector({json.dumps(sel)}); e.focus(); e.value = {json.dumps(text)};"
                " e.dispatchEvent(new Event('input', {bubbles: true})); })()")

    def slide(self, label: str, value: float):
        """Move the slider with this aria-label (or row label) to a value and let go."""
        self.wait(f"__t.find({json.dumps(label)}, 'input[type=range]')", what=f"slider {label!r}")
        self.js(f"(() => {{ const e = __t.find({json.dumps(label)}, 'input[type=range]'); e.value = {value};"
                " e.dispatchEvent(new Event('input', {bubbles: true})); e.dispatchEvent(new Event('change', {bubbles: true})); })()")

    def calls(self, method: str | None = None) -> list:
        c = self.js("window.__calls") or []
        return [x for x in c if not method or x[0] == method]

    def posts(self, path: str) -> list:
        return [b for m, u, b in self.calls("POST") if u.endswith("api/v1/" + path)]

    def close(self):
        try:
            os.close(self._w)
        except OSError:
            pass
        if getattr(self, "proc", None):
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        try:
            os.close(self._r)
        except OSError:
            pass
        self._prof.cleanup()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()
