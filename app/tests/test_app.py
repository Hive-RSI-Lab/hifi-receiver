#!/usr/bin/env python3
"""The web app's tests: the mock's API contract, the app's own files, the
firmware pack, and (when Chrome is on the machine) the app rendered against
the mock in a headless browser.

    python3 app/tests/test_app.py
"""
from __future__ import annotations

import contextlib
import io
import json
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

APP = Path(__file__).resolve().parent.parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(APP / "mock"))
sys.path.insert(0, str(APP))
import server  # noqa: E402
import build  # noqa: E402

# Straight to the mock, never through an HTTP proxy from the environment.
OPEN = urllib.request.build_opener(urllib.request.ProxyHandler({})).open
CHROME = next((shutil.which(c) for c in ("google-chrome", "chromium", "chromium-browser") if shutil.which(c)), None)


class Mock:
    def __init__(self, **kw):
        kw = {"model": "lite", "ap": False, "pdo": (20.0, 5.0), "fault": False, **kw}
        self.srv = server.serve(0, **kw)
        self.url = f"http://127.0.0.1:{self.srv.server_address[1]}/"
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def get(self, path):
        with OPEN(self.url + path) as r:
            return json.loads(r.read())

    def post(self, path, body):
        req = urllib.request.Request(self.url + "api/" + path, json.dumps(body).encode(),
                                     {"Content-Type": "application/json"})
        try:
            with OPEN(req) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            with e:
                return e.code, json.loads(e.read())

    def close(self):
        self.srv.shutdown()
        self.srv.server_close()


class API(unittest.TestCase):
    def setUp(self):
        self.m = Mock()

    def tearDown(self):
        self.m.close()

    def test_state_has_what_the_app_reads(self):
        s = self.m.get("api/state")
        for k in ("device", "caps", "power", "audio", "playing", "net", "update"):
            self.assertIn(k, s)
        self.assertEqual(len(s["audio"]["eq"]), len(s["caps"]["eq_bands"]))
        self.assertEqual(s["caps"]["zones"], 1)
        self.assertFalse(s["caps"]["sub"]["crossover"])

    def test_writes_answer_with_the_new_state(self):
        for path, body, check in (
            ("volume", {"volume": 70}, lambda s: s["audio"]["volume"] == 70),
            ("volume", {"volume": 140}, lambda s: s["audio"]["volume"] == 100),
            ("mute", {"mute": True}, lambda s: s["audio"]["mute"]),
            ("source", {"source": "line"}, lambda s: s["audio"]["source"] == "line"),
            ("line", {"mode": "adc"}, lambda s: s["audio"]["line_mode"] == "adc"),
            ("eq", {"gains": [3] * 10}, lambda s: s["audio"]["eq"] == [3] * 10),
            ("sub", {"db": -4.5, "on": False}, lambda s: s["audio"]["sub"] == {**s["audio"]["sub"], "db": -4.5, "on": False}),
            ("name", {"name": "Lounge"}, lambda s: s["device"]["name"] == "Lounge"),
        ):
            code, s = self.m.post(path, body)
            self.assertEqual(code, 200, (path, s))
            self.assertTrue(check(s), (path, body))
            self.assertEqual(self.m.get("api/state")["audio"], s["audio"])

    def test_bad_writes_are_refused(self):
        for path, body in (("volume", {"volume": "loud"}), ("mute", {"mute": 1}), ("source", {"source": "cd"}),
                           ("eq", {"gains": [0] * 3}), ("sub", {"xover_hz": 80}), ("zone", {"zone": "b", "on": True}),
                           ("name", {"name": ""}), ("reset", {}), ("transport", {"cmd": "skip"})):
            code, s = self.m.post(path, body)
            self.assertEqual(code, 400, (path, body))
            self.assertIn("error", s)
        self.assertEqual(self.m.post("nope", {})[0], 404)

    def test_wifi_join_and_forget(self):
        code, s = self.m.post("wifi", {"ssid": "Home", "password": "wrong"})
        self.assertEqual(s["net"]["join"]["state"], "failed")
        code, s = self.m.post("wifi", {"ssid": "Home", "password": "hunter22"})
        self.assertEqual(s["net"]["join"]["state"], "ok")
        code, s = self.m.post("wifi/forget", {})
        self.assertEqual(s["net"]["mode"], "ap")
        self.assertTrue(any(n["ssid"] == "Home" for n in self.m.get("api/wifi/scan")))

    def test_factory_reset_goes_back_to_setup(self):
        self.m.post("name", {"name": "Lounge"})
        code, s = self.m.post("reset", {"factory": True})
        self.assertEqual((code, s["net"]["mode"], s["device"]["name"]), (200, "ap", "HiFi Lite"))

    def test_selftest(self):
        code, r = self.m.post("selftest", {})
        self.assertTrue(r["ok"])
        self.assertTrue(all({"name", "ok"} <= set(i) for i in r["items"]))


class Models(unittest.TestCase):
    def test_plus_and_ultra_have_bass_management(self):
        for model, zones in (("plus", 1), ("ultra", 2)):
            m = Mock(model=model)
            try:
                code, s = m.post("sub", {"xover_hz": 100, "delay_ms": 3.2, "invert": True})
                self.assertEqual(code, 200)
                self.assertEqual((s["audio"]["sub"]["xover_hz"], s["audio"]["sub"]["delay_ms"]), (100, 3.2))
                self.assertEqual(m.post("zone", {"zone": "b", "on": True})[0], 200 if zones == 2 else 400)
            finally:
                m.close()

    def test_weak_charger_keeps_the_amp_off(self):
        m = Mock(pdo=(5.0, 3.0))
        try:
            self.assertFalse(m.get("api/state")["power"]["amp_on"])
        finally:
            m.close()


class Files(unittest.TestCase):
    def test_every_referenced_file_exists(self):
        refs = re.findall(r'(?:href|src)="([^"#:]+)"', (APP / "web/index.html").read_text())
        refs += [i["src"] for i in json.loads((APP / "web/manifest.webmanifest").read_text())["icons"]]
        refs += re.findall(r"'([\w.-]+\.\w+)'", re.search(r"FILES = \[(.*?)\]", (APP / "web/sw.js").read_text()).group(1))
        for r in refs:
            self.assertTrue((APP / "web" / r).is_file(), r)

    def test_every_endpoint_the_app_calls_is_in_the_mock(self):
        js = (APP / "web/app.js").read_text()
        used = set(re.findall(r"\b(?:api|set)\('([\w/]+)'", js)) | set(re.findall(r"'api/([\w/]+)'", js))
        src = (APP / "mock/server.py").read_text()
        for path in used:
            self.assertRegex(src, rf'"{re.escape(path)}"', path)

    @unittest.skipUnless(shutil.which("node"), "node not installed")
    def test_js_parses(self):
        for f in ("app.js", "sw.js"):
            subprocess.run(["node", "--check", str(APP / "web" / f)], check=True)


class Pack(unittest.TestCase):
    def test_pack_is_under_budget_and_the_same_every_time(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            for d in (a, b):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(build.main(["--out", d]), 0)
            self.assertEqual(Path(a, "webapp_files.c").read_bytes(), Path(b, "webapp_files.c").read_bytes())
            self.assertIn('"/index.html", "text/html"', Path(a, "webapp_files.c").read_text())
            cc = shutil.which("cc") or shutil.which("gcc")
            if cc:
                subprocess.run([cc, "-c", "-Wall", "-Werror", "-o", str(Path(a, "w.o")), str(Path(a, "webapp_files.c"))],
                               check=True)


@unittest.skipUnless(CHROME, "no Chrome on this machine")
class Browser(unittest.TestCase):
    """The app as a phone would load it, against the mock."""

    def dom(self, m, frag=""):
        with tempfile.TemporaryDirectory() as prof:
            r = subprocess.run([CHROME, "--headless=new", "--no-sandbox", "--disable-gpu", "--no-proxy-server", f"--user-data-dir={prof}",
                                "--virtual-time-budget=3000", "--dump-dom", m.url + frag],
                               capture_output=True, text=True, timeout=60)
        return r.stdout

    def test_play_page_shows_the_track_and_sources(self):
        m = Mock()
        try:
            d = self.dom(m)
            for want in ("Teardrop", "Massive Attack", "Spotify", "Line in", "Volume"):
                self.assertIn(want, d)
        finally:
            m.close()

    def test_first_run_opens_on_wifi_setup(self):
        m = Mock(ap=True)
        try:
            self.assertIn("Choose your home Wi-Fi", self.dom(m))
        finally:
            m.close()

    def test_weak_charger_banner(self):
        m = Mock(pdo=(5.0, 3.0))
        try:
            self.assertIn("charger too weak", self.dom(m))
        finally:
            m.close()


if __name__ == "__main__":
    unittest.main(verbosity=1)
