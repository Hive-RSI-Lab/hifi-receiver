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
            ("volume", {"volume": 90}, lambda s: s["audio"]["volume"] == 60),
            ("volume", {"mute": True}, lambda s: s["audio"]["mute"]),
            ("eq", {"gains": [3] * 10}, lambda s: s["audio"]["eq"] == [3] * 10),
            ("sub", {"level_db": -13, "on": False}, lambda s: (s["audio"]["sub"]["level_db"], s["audio"]["sub"]["on"]) == (-13.5, False)),
            ("ir/learn", {"action": "volup"}, lambda s: s["ir"]["learning"] == "volup"),
            ("ir/forget", {}, lambda s: s["ir"]["learned"] == []),
            ("loudness", {"boost_db": 6}, lambda s: s["audio"]["boost"]["db"] == 6),
            ("loudness", {"heat_cap_w": 12.5}, lambda s: s["governor"]["heat_cap_w"] == 12.5 and s["audio"]["boost"]["db"] == 6),
            ("channel", {"channel": "left"}, lambda s: s["audio"]["channel"] == "left"),
            ("channel", {"channel": "mono"}, lambda s: s["audio"]["channel"] == "mono"),
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
        for path, body in (("volume", {"volume": "loud"}), ("volume", {"mute": 1}), ("volume", {"volume": 140}),
                           ("volume", {"volume": 20.5}), ("volume", {"max_volume": 29}), ("volume", {}),
                           ("ir/forget", {"action": "eject"}), ("input", {"input": "line"}),
                           ("eq", {"gains": [0] * 3}), ("sub", {"xover_hz": 80}), ("sub", {"sub": 0, "trim_db": 1}),
                           ("zone", {"zone": "b", "on": True}), ("name", {"name": ""}), ("reset", {}),
                           ("control", {"action": "skip"}), ("ir/learn", {"action": "eject"}),
                           ("loudness", {}), ("loudness", {"boost_db": 10}), ("loudness", {"boost_db": "6"}),
                           ("loudness", {"heat_cap_w": 1}), ("loudness", {"boost_db": 3, "heat_cap_w": 40}),
                           ("channel", {}), ("channel", {"channel": "centre"}),
                           ("wifi", {"ssid": "x" * 31 + "\u00e9", "password": "p"}),  # 33 bytes
                           ("wifi", {"ssid": "Home", "password": "p" * 65}),
                           ("sub", {"level_db": -8.5})):  # over Lite's caps.sub.max_db
            code, s = self.m.post(path, body)
            self.assertEqual(code, 400, (path, body))
            self.assertIn("error", s)
        self.assertEqual(self.m.post("nope", {})[0], 404)
        # Lite has no LED test or backlight: 404, as the firmware
        self.assertEqual(self.m.post("led", {"mode": "test"})[0], 404)
        self.assertEqual(self.m.post("backlight", {"mode": "on"})[0], 404)

    def test_party_on_lead_off_and_no_key_from_the_lan(self):
        self.assertEqual(self.m.get("api/v1/party")["role"], "off")
        self.assertEqual(self.m.post("party", {"op": "lead"})[0], 409)  # party is off
        code, p = self.m.post("party", {"op": "on"})
        self.assertEqual((code, p["on"], p["role"]), (200, True, "leader"))
        code, p = self.m.post("party", {"op": "lead"})
        self.assertEqual((code, p["picked"]), (200, True))
        for body in ({"op": "key", "secret": "00" * 32}, {"op": "tone", "on": True}):
            self.assertEqual(self.m.post("party", body)[0], 403, body)  # the Hive's or USB's only
        self.assertEqual(self.m.post("party", {"op": "dance"})[0], 400)
        code, p = self.m.post("party", {"op": "off"})
        self.assertEqual((code, p["on"], p["role"]), (200, False, "off"))

    def test_wifi_join_and_forget(self):
        # as the firmware: {"ok": true} either way, and no net.join; the result shows in net
        self.m.srv.rx.s["net"].update(mode="ap", ssid="", ip="192.168.4.1")
        self.assertEqual(self.m.post("wifi", {"ssid": "Home", "password": "wrong"}), (200, {"ok": True}))
        net = self.m.get("api/v1/state")["net"]
        self.assertEqual((net["mode"], "join" in net), ("ap", False))
        self.assertEqual(self.m.post("wifi", {"ssid": "Home", "password": "hunter22"}), (200, {"ok": True}))
        net = self.m.get("api/v1/state")["net"]
        self.assertEqual((net["mode"], net["ssid"]), ("sta", "Home"))
        code, s = self.m.post("wifi/forget", {})
        self.assertEqual(s, {"ok": True})
        s = self.m.get("api/v1/state")
        self.assertEqual(s["net"]["mode"], "ap")
        self.assertTrue(any(n["ssid"] == "Home" for n in self.m.get("api/v1/wifi/scan")["networks"]))

    def test_factory_reset_goes_back_to_setup(self):
        self.m.post("name", {"name": "Lounge"})
        code, r = self.m.post("reset", {"factory": True})
        # from the LAN it only arms; nothing is erased until the unplug
        self.assertEqual((code, r["armed"], r["within_s"]), (200, True, 600))
        self.assertEqual(self.m.get("api/v1/info")["name"], "Lounge")
        self.m.srv.rx.unplug()
        self.assertEqual(self.m.get("api/v1/state")["net"]["mode"], "ap")
        self.assertEqual(self.m.get("api/v1/info")["name"], "HiFi Lite")

    def test_factory_reset_unarmed_unplug_keeps_settings(self):
        self.m.post("name", {"name": "Lounge"})
        self.m.srv.rx.unplug()
        self.assertEqual(self.m.get("api/v1/info")["name"], "Lounge")
        self.m.post("reset", {"factory": True})
        self.m.srv.rx.armed -= 601  # the window has gone by
        self.m.srv.rx.unplug()
        self.assertEqual(self.m.get("api/v1/info")["name"], "Lounge")

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
                # one volume for both zones on core: zone B has no level to set
                self.assertEqual(m.post("zone", {"zone": "b", "volume": 40})[0], 400)
                if zones == 2:
                    self.assertIsNone(m.get("api/v1/state")["audio"]["zone_b"]["volume"])
            finally:
                m.close()

    def test_each_model_has_its_own_settings_routes(self):
        # info.caps says which of channel, led and backlight a model answers (k2391)
        for model, want in (("lite", (True, False, False)), ("plus", (False, True, False)),
                            ("ultra", (False, True, True))):
            m = Mock(model=model)
            try:
                c = m.get("api/v1/info")["caps"]
                self.assertEqual((c["channel"], c["led"], c["backlight"]), want, model)
                self.assertEqual(m.post("channel", {"channel": "right"})[0], 200 if want[0] else 404)
                # Boost and the heat cap are Lite only (review of #231)
                self.assertEqual("loudness" in c, model == "lite")
                self.assertEqual(m.post("loudness", {"boost_db": 3})[0], 200 if model == "lite" else 404)
                code, r = m.post("led", {"mode": "test"})
                self.assertEqual((code, r), (200, {"ok": True}) if want[1] else (404, r))
                if want[2]:
                    self.assertEqual(m.post("backlight", {"mode": "off"}), (200, {"ok": True}))
                    self.assertEqual(m.get("api/v1/state")["controls"]["backlight"], "off")
                    self.assertEqual(m.post("backlight", {"mode": "dim"})[0], 400)
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

    def dom(self, m, frag="", ms=3000):
        with tempfile.TemporaryDirectory() as prof:
            r = subprocess.run([CHROME, "--headless=new", "--no-sandbox", "--disable-gpu", "--no-proxy-server", f"--user-data-dir={prof}",
                                f"--virtual-time-budget={ms}", "--dump-dom", m.url + frag],
                               capture_output=True, text=True, timeout=240)
        return r.stdout

    def test_play_page_shows_the_track_and_sources(self):
        m = Mock()
        try:
            d = self.dom(m)
            for want in ("Teardrop", "Massive Attack", "Spotify", "DJ direct", "Volume", "Party", "connecting your phone first"):
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

    def test_sound_page_has_the_loudness_sliders(self):
        m = Mock()
        try:
            d = self.dom(m, "#sound")
            for want in ("Loudness", "Boost", "Heat cap", "a fan helps", "Speaker output", "Mono"):
                self.assertIn(want, d)
            self.assertNotIn("Lights", self.dom(m, "#setup"))  # Lite has no LED test or backlight
        finally:
            m.close()

    def test_ultra_shows_lights_and_no_speaker_output(self):
        m = Mock(model="ultra")
        try:
            self.assertNotIn("Speaker output", self.dom(m, "#sound"))
            d = self.dom(m, "#setup")
            for want in ("Lights", "Backlight", "Light every LED"):
                self.assertIn(want, d)
        finally:
            m.close()

    def test_weak_charger_banner(self):
        m = Mock(pdo=(5.0, 3.0))
        try:
            self.assertIn("charger too weak", self.dom(m))
        finally:
            m.close()

    def test_power_card_on_play(self):
        m = Mock()
        try:
            d = self.dom(m)
            for want in ('aria-label="Power"', "20 V · 65 W charger", "Graph fills in · ", "Out", "Budget"):
                self.assertIn(want, d)
            self.assertNotIn("Limiting.", d)
            self.assertNotIn("Supply sagging.", d)
        finally:
            m.close()

    def test_power_card_says_when_it_limits(self):
        m = Mock()
        try:
            self.assertEqual(m.post("volume", {"volume": 95})[0], 200)
            d = self.dom(m)
            self.assertIn("Limiting.", d)
            self.assertIn("to stay within the 65 W charger", d)
            self.assertNotIn("Power limit.", d)  # said once, in the card
        finally:
            m.close()

    def test_power_card_says_when_the_supply_sags(self):
        m = Mock()
        rx = m.srv.rx
        power = rx._power
        def sag():
            power()
            rx.s["power"]["vbus_v"] = 16.8
        rx._power = sag
        try:
            d = self.dom(m)
            self.assertIn("Supply sagging.", d)
            self.assertIn("16.8 V from a 20 V charger", d)
        finally:
            m.close()

    def test_graphs_fill_in_and_diagnostics_has_them(self):
        m = Mock()
        try:
            d = self.dom(m, ms=40000)
            self.assertNotIn("Graph fills in", d)
            self.assertRegex(d, r'<path class="o" d="M[0-9.]+,[0-9.]+L')
            d = self.dom(m, "#setup", ms=12000)
            for want in ("Last 15 minutes", "Supply", "Wi-Fi signal", "Free memory", "low below 18.0 V",
                         "Touch a graph to read the values"):
                self.assertIn(want, d)
            self.assertRegex(d, r'<path class="v" d="M[0-9.]+,[0-9.]+L')
        finally:
            m.close()

    def test_sag_line_is_said_to_a_tenth_of_a_volt(self):
        m = Mock(pdo=(5.0, 3.0))
        try:
            self.assertIn("low below 4.5 V", self.dom(m, "#setup", ms=12000))
        finally:
            m.close()


if __name__ == "__main__":
    unittest.main(verbosity=1)
