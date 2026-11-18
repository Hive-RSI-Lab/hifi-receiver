#!/usr/bin/env python3
"""A mock receiver: serves app/web and answers /api/v1 the way the firmware
does, so the app can be built, tested and shown without a board. The shape is
the firmware's docs/test-api.md plus the calls only the app uses (docs/app.md,
"The API").

    python3 app/mock/server.py [--port 8080] [--model lite|plus|ultra]
                               [--ap] [--pdo 20x3.25] [--fault] [--host 0.0.0.0]

--ap starts in first-run setup (the receiver's own Wi-Fi), --pdo sets the
charger's contract (5x3 is a laptop port: the amplifier stays off), --fault
starts with the amplifier latched off. Only the standard library is used.
"""
from __future__ import annotations

import argparse
import copy
import json
import math
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

WEB = Path(__file__).resolve().parent.parent / "web"
EQ_BANDS = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
CONTROL = ("volup", "voldown", "toggle", "play", "pause", "stop", "next", "prev", "power", "mute", "input")
IR_ACTIONS = ("volup", "voldown", "mute", "toggle", "next", "prev", "input", "power")

CAPS = {
    "lite": {"line_modes": ["dj_direct"], "sub": {"count": 2, "crossover": False, "per_sub": False, "max_db": -9.0},
             "zones": 1, "eth": False, "pdo_v_max": 20, "channel": True, "led": False, "backlight": False},
    "plus": {"line_modes": ["dj_direct", "line"], "sub": {"count": 2, "crossover": True, "per_sub": True, "max_db": 0.0},
             "zones": 1, "eth": True, "pdo_v_max": 28, "channel": False, "led": True, "backlight": False},
    "ultra": {"line_modes": ["dj_direct", "line", "optical"],
              "sub": {"count": 2, "crossover": True, "per_sub": True, "max_db": 0.0},
              "zones": 2, "eth": True, "pdo_v_max": 28, "channel": False, "led": True, "backlight": True},
}
BOARDS = {"lite": "lite-main", "plus": "core", "ultra": "core+ultra-front"}
SCAN = [{"ssid": "Home", "rssi": -48, "secure": True},
        {"ssid": "Home-ext", "rssi": -63, "secure": True},
        {"ssid": "Neighbour", "rssi": -81, "secure": True},
        {"ssid": "Cafe guest", "rssi": -74, "secure": False}]
PASSWORDS = {"Home": "hunter22", "Home-ext": "hunter22", "Cafe guest": ""}
PLAYING = {"state": "play", "title": "Teardrop", "artist": "Massive Attack", "album": "Mezzanine",
           "elapsed_s": 61, "duration_s": 330}


def initial_info(model: str) -> dict:
    caps = copy.deepcopy(CAPS[model])
    caps.update(eq_bands=EQ_BANDS, eq_min=-12, eq_max=12)
    if model == "lite":  # Boost and the heat cap are Lite only (HIFI_HAS_BOOST)
        caps["loudness"] = {"boost_max_db": 9.0, "heat_cap_w": {"min": 2.0, "max": 15.0, "default": 9.0}}
    return {"model": "hifi-" + model, "board": BOARDS[model], "fw": "0.1.0",
            "upstream": "squeezelite-esp32 (mock)", "api": 1, "mac": "24:0a:c4:12:34:56",
            "hostname": "hifi-" + model, "name": "HiFi " + model.capitalize(),
            "flash_mb": 8, "psram_mb": 8, "caps": caps}


def initial_state(model: str, ap: bool, pdo: tuple[float, float], fault: bool) -> dict:
    v, a = pdo
    pd_ok = v >= 9
    sub = {"on": True, "level_db": 0.0}
    if CAPS[model]["sub"]["crossover"]:
        sub.update(xover_hz=80, mains_hp=False)
    if CAPS[model]["sub"]["per_sub"]:
        sub["subs"] = [{"trim_db": 0.0, "delay_ms": 0.0, "invert": False} for _ in range(2)]
    s = {
        "uptime_s": 0,
        "power": {"state": "on" if pd_ok else "wait_pd", "vbus_v": round(v * 1.006, 2), "pg": True,
                  "contract": {"protocol": "pd", "epr": v > 20, "v": v, "i_max_a": a, "w": round(v * a, 1)},
                  "pvdd_on": pd_ok, "pvdd_v": None, "vbus_i_a": None},
        "amp": {"enabled": pd_ok and not fault, "fault": False, "faults": 3 if fault else 0,
                "latched": fault, "temp_c": None if model == "lite" else 46, "hot": False},
        "governor": {"budget_w": round(0.9 * v * a, 1), "in_w": 7.9, "out_w": 4.4,
                     "gain_db": 0.0, "limiting": False, "limit_events": 0,
                     "reason": None, "heat_cap_w": 9.0, "heat_w": 2.2},
        "audio": {"source": "spotify", "input": "stream", "tone": None, "level_dbfs": -23.4,
                  "volume": 35, "mute": False, "max_volume": 100,
                  "boost": {"db": 0.0, "limit_db": 0.0, "limiting": False, "limit_events": 0},
                  "eq": [0] * len(EQ_BANDS), "channel": "stereo",
                  "sub": sub, "playing": dict(PLAYING)},
        "net": {"mode": "sta", "ssid": "Home", "ip": "192.168.1.40", "rssi": -52,
                "hostname": "hifi-" + model, "eth": model != "lite"},
        "sys": {"heap_free": 81234, "psram_free": 3912345},
        "update": {"state": "idle", "progress": 0},
        "ir": {"learning": None, "learned": [], "last_code": None},
    }
    if CAPS[model]["backlight"]:
        s["controls"] = {"last": None, "backlight": "auto"}
    if CAPS[model]["zones"] > 1:
        # core's zone B amp takes zone A's signal: it has no level of its own
        s["audio"]["zone_b"] = {"on": False, "volume": None}
    if ap:
        s["net"].update(mode="ap", ssid="", rssi=None, ip="192.168.4.1", eth=False)
        s["audio"].update(source="idle", playing=None)
    return s


ARM_S = 600  # an armed factory reset waits this long for the unplug

class Receiver:
    """The state and the rules the firmware keeps, with a lock for the
    threaded server."""

    def __init__(self, model="lite", ap=False, pdo=(20.0, 3.25), fault=False):
        self.model, self.pdo = model, pdo
        self.info = initial_info(model)
        self.s = initial_state(model, ap, pdo, fault)
        self.lock = threading.Lock()
        self.armed = None  # when a factory reset was armed
        self.fail = {}  # tests: path -> (HTTP code, error) to answer instead (code None: never answer), as a receiver that can't
        self.t0 = time.time() - (5 * 3600 + 17 * 60)
        # party mode (GET/POST party): the mock's one other receiver, Kitchen,
        # is on and waiting, so switching party on here makes this one lead
        self.party = {"on": False, "role": "off", "key_id": 42, "key": True, "term": 0, "picked": False,
                      "followers": [], "why": "", "bt": True}

    def party_op(self, b: dict) -> dict:
        """POST party from the LAN: op on, off or lead (key and tone are the
        Hive's or USB's only: 403)."""
        with self.lock:
            p, op = self.party, b.get("op")
            if op in ("key", "tone"):
                raise Forbidden("the party key and the test tone are set by the Hive or over USB, not from the LAN")
            if op == "on":
                p.update(on=True, role="leader", leader=self.info["name"], followers=["192.168.1.31"], term=p["term"] + 1)
            elif op == "off":
                p.update(on=False, role="off", followers=[], why="")
                p.pop("leader", None)
            elif op == "lead":
                if not p["on"]:
                    raise Conflict("party is off")
                p.update(role="leader", picked=True, leader=self.info["name"], followers=["192.168.1.31"], term=p["term"] + 1)
            else:
                raise ValueError("op must be on, off, lead, key or tone")
            return copy.deepcopy(p)

    def state(self) -> dict:
        with self.lock:
            self._tick()
            self._power()
            return copy.deepcopy(self.s)

    def _power(self):
        """Move the power numbers a little on each poll, so the app's graphs
        have something to draw: music swings with the volume, Wi-Fi and heap wander."""
        self.n = n = getattr(self, "n", 0) + 1
        s, g, a = self.s, self.s["governor"], self.s["audio"]
        if s["power"]["state"] != "on" or a["mute"] or a["source"] == "idle":
            out = 0.0
        else:
            beat = 0.55 + 0.3 * math.sin(n / 7) + 0.15 * math.sin(n * 1.7)
            out = 90 * (a["volume"] / 100) ** 2 * beat * 10 ** (g["gain_db"] / 20)
        if g["limiting"]:  # the governor holds the draw at its budget
            out = min(out, (g["budget_w"] * (0.96 + 0.03 * math.sin(n)) - 2.4) * 0.86)
        g["out_w"] = round(out, 1)
        g["in_w"] = round(2.4 + out / 0.86, 1)
        if g["limiting"] and n % 9 == 0:
            g["limit_events"] += 1
        c = s["power"]["contract"]
        s["power"]["vbus_v"] = round(c["v"] * 1.006 - 0.004 * g["in_w"], 2)
        if s["net"]["rssi"] is not None:
            s["net"]["rssi"] = -52 + round(3 * math.sin(n / 23))
        s["sys"]["heap_free"] = 81234 - 600 * (n % 40) // 4

    def _tick(self):
        s = self.s
        s["uptime_s"] = int(time.time() - self.t0)
        # The governor, roughly: loud playing on a small charger is turned down.
        g, a = s["governor"], s["audio"]
        g["gain_db"] = -round(max(0.0, (a["volume"] - 80) * 0.3), 1) if g["budget_w"] < 60 else 0.0
        g["limiting"] = g["gain_db"] < 0
        # An IR learn ends with the next key the remote sends; here, at once.
        ir = s["ir"]
        if ir["learning"] and time.time() - ir.get("_since", 0) > 3:
            ir["learned"] = [x for x in IR_ACTIONS if x in set(ir["learned"]) | {ir["learning"]}]
            ir["learning"] = None
            ir["last_code"] = "0x00ff40bf"

    def post(self, path: str, b: dict) -> dict:
        """Apply one write; return the reply. Raises ValueError for a request
        the firmware refuses (400), Conflict for one it can't do now (409),
        KeyError for no such path (404)."""
        with self.lock:
            s = self.s
            a, caps = s["audio"], self.info["caps"]
            if path == "volume":
                # as the firmware: whole numbers in range or a 400 that changes
                # nothing; null leaves a field; the maximum first, the volume held to it
                vol, mx = (whole(b, "volume", 0, 100), whole(b, "max_volume", 30, 100))
                mute = boolean(b, "mute") if b.get("mute") is not None else None
                if vol is None and mx is None and mute is None:
                    raise ValueError("send volume, mute and/or max_volume")
                if mx is not None:
                    a["max_volume"] = mx
                    a["volume"] = min(a["volume"], mx)
                if vol is not None:
                    a["volume"] = min(vol, a["max_volume"])
                if mute is not None:
                    a["mute"] = mute
            elif path == "input":
                if b.get("input") not in ["stream"] + caps["line_modes"]:
                    raise ValueError("input must be one of " + ", ".join(["stream"] + caps["line_modes"]))
                a["input"] = b["input"]
                self._tick()
                return {"ok": True, "input": a["input"]}  # the firmware's reply (test-api.md)
            elif path == "eq":
                g = b.get("gains")
                if not isinstance(g, list) or len(g) != len(caps["eq_bands"]):
                    raise ValueError(f"gains must be a list of {len(caps['eq_bands'])} numbers")
                a["eq"] = [clamp(num({"g": x}, "g"), caps["eq_min"], caps["eq_max"], int) for x in g]
            elif path == "sub":
                self._sub(b, a["sub"], caps["sub"])
            elif path == "loudness":
                # as the firmware: both checked before either is set
                if "loudness" not in caps:
                    raise KeyError(path)  # 404, as the firmware on a board without Boost
                L = caps["loudness"]
                bd = None if b.get("boost_db") is None else num(b, "boost_db")
                hw = None if b.get("heat_cap_w") is None else num(b, "heat_cap_w")
                if bd is None and hw is None:
                    raise ValueError("send boost_db and/or heat_cap_w")
                if bd is not None and not 0 <= bd <= L["boost_max_db"]:
                    raise ValueError("boost_db is out of range")
                if hw is not None and not L["heat_cap_w"]["min"] <= hw <= L["heat_cap_w"]["max"]:
                    raise ValueError("heat_cap_w is out of range")
                if bd is not None:
                    a["boost"]["db"] = round(bd * 10) / 10
                    a["boost"]["limiting"] = bd > 0
                    a["boost"]["limit_db"] = -round(bd / 3, 1)
                if hw is not None:
                    s["governor"]["heat_cap_w"] = round(hw * 2) / 2
            elif path == "channel":
                # as the firmware: Lite's ES8388 mixers pick the side; 404 elsewhere
                if not caps["channel"]:
                    raise KeyError(path)
                if b.get("channel") not in ("stereo", "left", "right", "mono"):
                    raise ValueError("channel must be stereo, left, right or mono")
                a["channel"] = b["channel"]
            elif path in ("led", "backlight"):
                modes = ("test", "auto") if path == "led" else ("on", "off", "auto")
                if not caps[path]:
                    raise KeyError(path)
                if b.get("mode") not in modes:
                    raise ValueError("mode must be " + ", ".join(modes[:-1]) + " or " + modes[-1])
                if path == "backlight":
                    s["controls"]["backlight"] = b["mode"]
                return {"ok": True}
            elif path == "zone":
                if caps["zones"] < 2 or b.get("zone") != "b":
                    raise ValueError("no such zone")
                z = a["zone_b"]
                if "on" in b:
                    z["on"] = boolean(b, "on")
                if "volume" in b:
                    raise ValueError("zone B has no level of its own: it plays at the main volume")
            elif path == "control":
                act = b.get("action")
                if act not in CONTROL:
                    raise ValueError("action must be one of " + ", ".join(CONTROL))
                if act in ("volup", "voldown"):
                    a["volume"] = clamp(a["volume"] + (5 if act == "volup" else -5), 0, a["max_volume"], int)
                    a["mute"] = False
                elif act == "mute":
                    a["mute"] = not a["mute"]
                elif act == "input":
                    a["input"] = "stream" if a["input"] != "stream" else caps["line_modes"][0]
                elif a["playing"]:
                    st = a["playing"]["state"]
                    a["playing"]["state"] = {"play": "play", "pause": "pause", "stop": "stop",
                                             "toggle": "pause" if st == "play" else "play"}.get(act, st)
                return {"ok": True}
            elif path == "amp":
                amp = s["amp"]
                if b.get("reset"):
                    amp.update(latched=False, fault=False, faults=0)
                if "enable" in b:
                    amp["enabled"] = boolean(b, "enable")
                amp["enabled"] = amp["enabled"] and not amp["latched"] and s["power"]["state"] == "on"
                return copy.deepcopy(amp)
            elif path == "tone":
                if s["amp"]["latched"] or s["power"]["state"] != "on":
                    if not b.get("force"):
                        raise Conflict("the amp is off: " + ("latched" if s["amp"]["latched"] else s["power"]["state"]))
                a["tone"] = None if b.get("type") == "stop" else b
                return {"ok": True, "tone": a["tone"]}
            elif path == "name":
                # as the firmware's r_name: no trimming, and nothing that would break squeezelite's -n "<name>"
                name = b.get("name")
                if not isinstance(name, str) or not name:
                    raise ValueError("name is required")
                if len(name.encode()) > 32:
                    raise ValueError("name must be 32 characters or fewer")
                if any(ord(c) < 0x20 or ord(c) == 0x7f for c in name):
                    raise ValueError("name has a control character")
                if '"' in name or "\\" in name:
                    raise ValueError('name may not have " or \\ in it')
                self.info["name"] = name
            elif path == "wifi":
                ssid = str(b.get("ssid", ""))
                if not ssid:
                    raise ValueError("ssid is required")
                # as the firmware: too long is refused, never joined cut short
                if len(ssid.encode()) > 32:
                    raise ValueError("ssid must be 32 bytes or fewer")
                if len(str(b.get("password", "")).encode()) > 64:
                    raise ValueError("password must be 64 characters or fewer")
                # as the firmware: {"ok": true} at once, and the result shows
                # only in net (on that network, or not); a wrong password changes nothing
                if PASSWORDS.get(ssid) == b.get("password", ""):
                    s["net"].update(mode="sta", ssid=ssid, ip="192.168.1.40",
                                    rssi=next((n["rssi"] for n in SCAN if n["ssid"] == ssid), -60))
                return {"ok": True}
            elif path == "wifi/forget":
                s["net"].update(mode="ap", ssid="", rssi=None, ip="192.168.4.1")
                return {"ok": True}
            elif path == "ir/learn":
                if b.get("action") not in IR_ACTIONS:
                    raise ValueError("action must be one of " + ", ".join(IR_ACTIONS))
                s["ir"].update(learning=b["action"], _since=time.time())
            elif path == "ir/forget":
                act = b.get("action")
                if act is not None and act not in IR_ACTIONS:
                    raise ValueError("action must be one of " + ", ".join(IR_ACTIONS))
                s["ir"]["learned"] = [x for x in s["ir"]["learned"] if act is not None and x != act]
                s["ir"]["learning"] = None
            elif path == "update":
                if not str(b.get("url", "")).startswith(("http://", "https://")):
                    raise ValueError("url must be http(s)")
                s["update"] = {"state": "downloading", "progress": 0}
                threading.Thread(target=self._fake_update, daemon=True).start()
            elif path == "reboot":
                self.t0 = time.time()
            elif path == "reset":
                if b.get("factory") is not True:
                    raise ValueError('send {"factory": true} to erase the settings')
                # from the LAN the firmware only arms it: unplugging the
                # receiver within ARM_S finishes it (security.md L6)
                self.armed = time.time()
                return {"armed": True, "within_s": ARM_S, "next": "unplug the receiver and plug it back in"}
            else:
                raise KeyError(path)
            self._tick()
            return self._public(self.s)

    def unplug(self):
        """Power off and on: an armed factory reset erases now."""
        with self.lock:
            if self.armed is not None and time.time() - self.armed < ARM_S:
                self.info = initial_info(self.model)
                self.s = initial_state(self.model, True, self.pdo, False)
            self.armed = None
            self.t0 = time.time()

    def _sub(self, b, sub, c):
        if "sub" in b:  # one sub's own settings
            i = b["sub"]
            if not c["per_sub"] or not isinstance(i, int) or not 0 <= i < c["count"]:
                raise ValueError("no such sub")
            one = sub["subs"][i]
            if "trim_db" in b:
                one["trim_db"] = clamp(num(b, "trim_db"), -12, 6, float)
            if "delay_ms" in b:
                one["delay_ms"] = clamp(num(b, "delay_ms"), 0, 20, float)
            if "invert" in b:
                one["invert"] = boolean(b, "invert")
            return
        if "level_db" in b:  # the ES8388's LOUT2/ROUT2 in 1.5 dB steps on Lite (test-api.md)
            v = num(b, "level_db")
            if not -30 <= v <= c["max_db"]:
                raise ValueError(f"level_db must be -30 to {c['max_db']:+.1f} on this model")
            sub["level_db"] = round(v / 1.5) * 1.5
        if "on" in b:
            sub["on"] = boolean(b, "on")
        for k, lo, hi in (("xover_hz", 40, 160),):
            if k in b:
                if not c["crossover"]:
                    raise ValueError(f"{k} is not on this model")
                sub[k] = clamp(num(b, k), lo, hi, int)
        if "mains_hp" in b:
            if not c["crossover"]:
                raise ValueError("mains_hp is not on this model")
            sub["mains_hp"] = boolean(b, "mains_hp")

    @staticmethod
    def _public(s):
        out = copy.deepcopy(s)
        out["ir"].pop("_since", None)
        return out

    def selftest(self) -> dict:
        s = self.s
        p, c = s["power"], s["power"]["contract"]
        on = p["state"] == "on"
        checks = [
            {"name": "pd_sink_i2c", "pass": True, "value": "0x22 acks"},
            {"name": "pd_contract", "pass": on, "value": f"pd {c['v']} V {c['i_max_a']} A"},
            {"name": "vbus_matches", "pass": True, "value": f"{p['vbus_v']} V"},
            {"name": "codec_i2c", "pass": True, "value": "0x10 acks"},
            {"name": "pvdd_on", "pass": on, "value": "on" if on else "off: " + p["state"]},
            {"name": "amp_no_fault", "pass": on and not s["amp"]["latched"],
             "value": "latched" if s["amp"]["latched"] else "FAULTZ high after 200 ms enabled"},
            {"name": "loopback", "pass": on and not s["amp"]["latched"],
             "value": "stereo: L +0.1/-43.4, R -42.1/-0.1 dB" if on and not s["amp"]["latched"] else "amp not on"},
            {"name": "psram", "pass": True, "value": "8 MB"},
            {"name": "heap", "pass": True, "value": f"{s['sys']['heap_free']} B free"},
            {"name": "network", "pass": s["net"]["mode"] == "sta",
             "value": s["net"]["ip"] if s["net"]["mode"] == "sta" else "not connected"},
        ]
        return {"pass": all(x["pass"] for x in checks), "checks": checks}

    def _fake_update(self):
        for st, step in (("downloading", 20), ("flashing", 25)):
            for pct in range(0, 101, step):
                with self.lock:
                    self.s["update"] = {"state": st, "progress": pct}
                time.sleep(0.3)
        with self.lock:
            self.s["update"] = {"state": "done", "progress": 100}
        time.sleep(1)
        with self.lock:  # the receiver restarts into the new firmware
            self.s["update"] = {"state": "idle", "progress": 0}
            self.info["fw"] = "0.2.0"
            self.t0 = time.time()


class Conflict(Exception):
    pass


class Forbidden(Exception):
    pass


def num(b, k):
    v = b.get(k)
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise ValueError(f"{k} must be a number")
    return v


def whole(b, k, lo, hi):
    if b.get(k) is None:
        return None
    v = num(b, k)
    if v != int(v) or not lo <= v <= hi:
        raise ValueError(f"{k} must be a whole number {lo} to {hi}")
    return int(v)


def boolean(b, k):
    if not isinstance(b.get(k), bool):
        raise ValueError(f"{k} must be true or false")
    return b[k]


def clamp(v, lo, hi, kind):
    return kind(min(hi, max(lo, round(v, 1) if kind is float else round(v))))


def handler(rx: Receiver):
    class H(SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=str(WEB), **kw)

        def log_message(self, *a):
            pass

        def send(self, code, obj):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_OPTIONS(self):
            self.send_response(204)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, POST")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.end_headers()

        def api(self):
            p = self.path.split("?")[0]
            return p[len("/api/v1/"):] if p.startswith("/api/v1/") else None

        def failed(self, p):
            if p in rx.fail:
                code, msg = rx.fail[p]
                if code is None:  # a receiver that restarted mid-request: no answer at all
                    time.sleep(30)
                    self.close_connection = True
                    return True
                self.send(code, {"error": msg})
                return True
            return False

        def do_GET(self):
            p = self.api()
            if p is None:
                return super().do_GET()
            if self.failed(p):
                return
            if p == "info":
                with rx.lock:
                    return self.send(200, copy.deepcopy(rx.info))
            if p == "state":
                return self.send(200, Receiver._public(rx.state()))
            if p == "party":
                with rx.lock:
                    return self.send(200, copy.deepcopy(rx.party))
            if p == "wifi/scan":
                return self.send(200, {"networks": SCAN})
            if p == "update/check":
                return self.send(200, {"current": rx.info["fw"], "latest": "0.2.0",
                                       "url": "https://example.invalid/hifi-lite-0.2.0.bin",
                                       "notes": "Faster Wi-Fi reconnect."})
            self.send(404, {"error": "no such path"})

        def do_POST(self):
            p = self.api()
            n = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(n)
            if self.failed(p):
                return
            if p == "update/upload":
                if n < 1024:
                    return self.send(400, {"error": "that file is too small to be firmware"})
                with rx.lock:
                    rx.s["update"] = {"state": "flashing", "progress": 0}
                threading.Thread(target=rx._fake_update, daemon=True).start()
                return self.send(200, Receiver._public(rx.state()))
            try:
                body = json.loads(raw or b"{}")
                if not isinstance(body, dict):
                    raise ValueError("body must be a JSON object")
                if p == "selftest":
                    with rx.lock:
                        return self.send(200, rx.selftest())
                if p == "party":
                    return self.send(200, rx.party_op(body))
                return self.send(200, rx.post(p, body))
            except KeyError:
                self.send(404, {"error": "no such path"})
            except Conflict as e:
                self.send(409, {"error": str(e)})
            except Forbidden as e:
                self.send(403, {"error": str(e)})
            except (ValueError, json.JSONDecodeError) as e:
                self.send(400, {"error": str(e)})
    return H


def serve(port=8080, host="127.0.0.1", **kw) -> ThreadingHTTPServer:
    rx = Receiver(**kw)
    srv = ThreadingHTTPServer((host, port), handler(rx))
    srv.rx = rx
    srv.daemon_threads = True
    return srv


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="127.0.0.1", help="0.0.0.0 to reach it from a phone on the same Wi-Fi")
    ap.add_argument("--model", choices=sorted(CAPS), default="lite")
    ap.add_argument("--ap", action="store_true", help="start in first-run Wi-Fi setup")
    ap.add_argument("--pdo", default="20x3.25", help="charger contract, volts x amps (default 20x3.25, a 65 W charger)")
    ap.add_argument("--fault", action="store_true", help="start with the amplifier latched off")
    o = ap.parse_args()
    v, a = (float(x) for x in o.pdo.lower().split("x"))
    srv = serve(o.port, o.host, model=o.model, ap=o.ap, pdo=(v, a), fault=o.fault)
    print(f"mock {o.model} receiver on http://{o.host}:{o.port}/", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
