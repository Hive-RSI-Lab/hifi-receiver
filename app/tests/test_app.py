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
        kw = {"model": "lite", "ap": False, "pdo": (20.0, 3.25), "fault": False, **kw}
        self.srv = server.serve(0, **kw)
        self.url = f"http://127.0.0.1:{self.srv.server_address[1]}/"
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def get(self, path):
        with OPEN(self.url + path) as r:
            return json.loads(r.read())

    def post(self, path, body):
        req = urllib.request.Request(self.url + "api/v1/" + path, json.dumps(body).encode(),
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

    def test_info_and_state_have_what_the_app_reads(self):
        i = self.m.get("api/v1/info")
        for k in ("model", "fw", "mac", "hostname", "name", "caps"):
            self.assertIn(k, i)
        s = self.m.get("api/v1/state")
        # the firmware's own fields (docs/test-api.md)
        for k in ("uptime_s", "power", "amp", "governor", "audio", "net", "sys"):
            self.assertIn(k, s)
        self.assertEqual(set(s["power"]["contract"]), {"protocol", "epr", "v", "i_max_a", "w"})
        # and the app's (docs/app.md)
        for k in ("volume", "mute", "max_volume", "eq", "sub", "playing", "input", "source"):
            self.assertIn(k, s["audio"])
        self.assertEqual(len(s["audio"]["eq"]), len(i["caps"]["eq_bands"]))
        self.assertEqual(i["caps"]["line_modes"], ["dj_direct"])

    def test_writes_answer_with_the_new_state(self):
        for path, body, check in (
            ("volume", {"volume": 70}, lambda s: s["audio"]["volume"] == 70),
            ("volume", {"max_volume": 60}, lambda s: s["audio"]["volume"] == 60),
            ("volume", {"volume": 140}, lambda s: s["audio"]["volume"] == 60),
            ("volume", {"mute": True}, lambda s: s["audio"]["mute"]),
            ("eq", {"gains": [3] * 10}, lambda s: s["audio"]["eq"] == [3] * 10),
            ("sub", {"level_db": -4, "on": False}, lambda s: (s["audio"]["sub"]["level_db"], s["audio"]["sub"]["on"]) == (-4.5, False)),
            ("ir/learn", {"action": "volup"}, lambda s: s["ir"]["learning"] == "volup"),
            ("ir/forget", {}, lambda s: s["ir"]["learned"] == []),
        ):
            code, s = self.m.post(path, body)
            self.assertEqual(code, 200, (path, s))
            self.assertTrue(check(s), (path, body, s["audio"]))
            self.assertEqual(self.m.get("api/v1/state")["audio"], s["audio"])
        code, _ = self.m.post("name", {"name": "Lounge"})
        self.assertEqual(self.m.get("api/v1/info")["name"], "Lounge")

    def test_the_firmware_calls_reply_as_test_api_says(self):
        self.assertEqual(self.m.post("input", {"input": "dj_direct"}), (200, {"ok": True, "input": "dj_direct"}))
        self.assertEqual(self.m.post("control", {"action": "pause"}), (200, {"ok": True}))
        self.assertEqual(self.m.get("api/v1/state")["audio"]["playing"]["state"], "pause")
        code, amp = self.m.post("amp", {"enable": False})
        self.assertEqual((code, amp["enabled"]), (200, False))
        code, r = self.m.post("tone", {"type": "sine", "freq_hz": 1000, "level_dbfs": -20})
        self.assertEqual((code, r["ok"]), (200, True))

    def test_bad_writes_are_refused(self):
        for path, body in (("volume", {"volume": "loud"}), ("volume", {"mute": 1}), ("input", {"input": "line"}),
                           ("eq", {"gains": [0] * 3}), ("sub", {"xover_hz": 80}), ("sub", {"sub": 0, "trim_db": 1}),
                           ("zone", {"zone": "b", "on": True}), ("name", {"name": ""}), ("reset", {}),
                           ("control", {"action": "skip"}), ("ir/learn", {"action": "eject"})):
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
        self.assertTrue(any(n["ssid"] == "Home" for n in self.m.get("api/v1/wifi/scan")["networks"]))

    def test_factory_reset_goes_back_to_setup(self):
        self.m.post("name", {"name": "Lounge"})
        code, s = self.m.post("reset", {"factory": True})
        self.assertEqual((code, s["net"]["mode"]), (200, "ap"))
        self.assertEqual(self.m.get("api/v1/info")["name"], "HiFi Lite")

    def test_selftest(self):
        code, r = self.m.post("selftest", {})
        self.assertTrue(r["pass"])
        self.assertTrue(all({"name", "pass", "value"} <= set(c) for c in r["checks"]))


class Models(unittest.TestCase):
    def test_plus_and_ultra_have_bass_management(self):
        for model, zones in (("plus", 1), ("ultra", 2)):
            m = Mock(model=model)
            try:
                code, s = m.post("sub", {"xover_hz": 100, "mains_hp": True})
                self.assertEqual((code, s["audio"]["sub"]["xover_hz"]), (200, 100))
                code, s = m.post("sub", {"sub": 1, "delay_ms": 3.2, "invert": True})
                self.assertEqual(s["audio"]["sub"]["subs"][1], {"trim_db": 0.0, "delay_ms": 3.2, "invert": True})
                self.assertEqual(m.post("zone", {"zone": "b", "on": True})[0], 200 if zones == 2 else 400)
            finally:
                m.close()

    def test_weak_charger_keeps_the_amp_off(self):
        m = Mock(pdo=(5.0, 3.0))
        try:
            s = m.get("api/v1/state")
            self.assertEqual((s["power"]["state"], s["amp"]["enabled"]), ("wait_pd", False))
            self.assertEqual(m.post("tone", {"type": "sine"})[0], 409)
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
        used = set(re.findall(r"\b(?:api|set)\('([\w/]+)'", js)) | set(re.findall(r"'api/v1/([\w/]+)'", js))
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
                               capture_output=True, text=True, timeout=240)
        return r.stdout

    def test_play_page_shows_the_track_and_sources(self):
        m = Mock()
        try:
            d = self.dom(m)
            for want in ("Teardrop", "Massive Attack", "Spotify", "DJ direct", "Volume"):
                self.assertIn(want, d)
        finally:
            m.close()

    def test_first_run_opens_on_wifi_setup(self):
        m = Mock(ap=True)
        try:
            self.assertIn("Choose your home Wi-Fi", self.dom(m))
        finally:
            m.close()

    def test_setup_page_has_wifi_remote_and_updates(self):
        m = Mock()
        try:
            d = self.dom(m, "#setup")
            for want in ("Scan for networks", "Teach", "Check for updates", "Factory reset"):
                self.assertIn(want, d)
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
