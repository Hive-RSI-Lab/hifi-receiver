#!/usr/bin/env python3
"""A mock receiver: serves app/web and answers the app's API the way the
firmware does (docs/test-api.md, "The app's API"), so the app can be built,
tested and shown without a board.

    python3 app/mock/server.py [--port 8080] [--model lite|plus|ultra]
                               [--ap] [--pdo 20x5] [--fault]

--ap starts in first-run setup (the receiver's own Wi-Fi), --pdo sets the
charger's contract (5x3 is a laptop port: the amplifier stays off), --fault
starts with an amplifier fault. Only the standard library is used.
"""
from __future__ import annotations

import argparse
import copy
import json
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

WEB = Path(__file__).resolve().parent.parent / "web"
EQ_BANDS = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
STREAMS = ["spotify", "airplay", "bt", "lms"]

CAPS = {
    "lite": {"sources": STREAMS + ["line"], "line_modes": ["direct", "adc"],
             "sub": {"crossover": False, "delay": False, "phase": False, "mains_hp": False},
             "zones": 1, "eth": False, "pdo_v_max": 20},
    "plus": {"sources": STREAMS + ["line"], "line_modes": ["direct", "adc"],
             "sub": {"crossover": True, "delay": True, "phase": True, "mains_hp": True},
             "zones": 1, "eth": True, "pdo_v_max": 28},
    "ultra": {"sources": STREAMS + ["line", "optical"], "line_modes": ["direct", "adc"],
              "sub": {"crossover": True, "delay": True, "phase": True, "mains_hp": True},
              "zones": 2, "eth": True, "pdo_v_max": 28},
}
NAMES = {"lite": "HiFi Lite", "plus": "HiFi Plus", "ultra": "HiFi Ultra"}
SCAN = [{"ssid": "Home", "rssi": -48, "secure": True},
        {"ssid": "Home-5G-ext", "rssi": -63, "secure": True},
        {"ssid": "Neighbour", "rssi": -81, "secure": True},
        {"ssid": "Cafe guest", "rssi": -74, "secure": False}]
PASSWORDS = {"Home": "hunter22", "Home-5G-ext": "hunter22", "Cafe guest": ""}
PLAYING = {"title": "Teardrop", "artist": "Massive Attack", "album": "Mezzanine"}


def initial(model: str, ap: bool, pdo: tuple[float, float], fault: bool) -> dict:
    caps = copy.deepcopy(CAPS[model])
    caps.update(eq_bands=EQ_BANDS, eq_min=-12, eq_max=12, transport=["spotify", "airplay", "bt", "lms"])
    v, a = pdo
    s = {
        "device": {"name": NAMES[model], "model": model, "model_name": NAMES[model],
                   "fw": "0.1.0", "mac": "24:6f:28:a1:b2:c3", "uptime_s": 0},
        "caps": caps,
        "power": {"pdo_v": v, "pdo_a": a, "vbus_v": v * 0.99, "amp_on": v >= 9 and not fault,
                  "amp_fault": fault, "limit_db": 0.0, "temp_c": 41},
        "audio": {"volume": 35, "mute": False, "source": "spotify", "line_mode": "direct",
                  "eq": [0] * len(EQ_BANDS),
                  "sub": {"on": True, "db": 0.0, "xover_hz": 80, "delay_ms": 0.0, "invert": False,
                          "mains_hp": False}},
        "playing": {"state": "play", **PLAYING},
        "net": {"mode": "sta", "ssid": "Home", "rssi": -52, "ip": "192.168.1.42",
                "hostname": "hifi-" + model, "eth": model != "lite", "join": None},
        "update": {"state": "idle", "progress": 0},
    }
    if caps["zones"] > 1:
        s["audio"]["zone_b"] = {"on": False, "volume": 30}
    if ap:
        s["net"].update(mode="ap", ssid="", rssi=0, ip="192.168.4.1", eth=False)
        s["audio"]["source"] = "idle"
        s["playing"] = {"state": "stop"}
    return s


class Receiver:
    """The state and the rules the firmware keeps, with a lock for the
    threaded server."""

    def __init__(self, **kw):
        self.s = initial(**kw)
        self.lock = threading.Lock()
        self.t0 = time.time()

    def state(self) -> dict:
        with self.lock:
            self.s["device"]["uptime_s"] = int(time.time() - self.t0) + 3600 * 5 + 17 * 60
            # The governor, roughly: a small charger turns loud playing down.
            p, vol = self.s["power"], self.s["audio"]["volume"]
            p["limit_db"] = round(max(0.0, (vol - 80) * 0.3), 1) if p["pdo_v"] * p["pdo_a"] < 65 else 0.0
            return copy.deepcopy(self.s)

    def post(self, path: str, b: dict) -> dict:
        """Apply one write; return the new state. Raises ValueError for a
        request the firmware would refuse (400)."""
        with self.lock:
            s, a, caps = self.s, self.s["audio"], self.s["caps"]
            if path == "volume":
                a["volume"] = clamp(num(b, "volume"), 0, 100, int)
            elif path == "mute":
                a["mute"] = boolean(b, "mute")
            elif path == "source":
                src = b.get("source")
                if src not in caps["sources"] + ["idle"]:
                    raise ValueError(f"unknown source {src!r}")
                a["source"] = src
                s["playing"] = ({"state": "play", **PLAYING} if src in caps["transport"]
                                else {"state": "play" if src != "idle" else "stop"})
            elif path == "line":
                if b.get("mode") not in caps["line_modes"]:
                    raise ValueError("mode must be one of " + ", ".join(caps["line_modes"]))
                a["line_mode"] = b["mode"]
            elif path == "eq":
                g = b.get("gains")
                if not isinstance(g, list) or len(g) != len(caps["eq_bands"]):
                    raise ValueError(f"gains must be a list of {len(caps['eq_bands'])} numbers")
                a["eq"] = [clamp(float(x), caps["eq_min"], caps["eq_max"], int) for x in g]
            elif path == "sub":
                sub, c = a["sub"], caps["sub"]
                for k, lo, hi, need in (("db", -15, 10, None), ("xover_hz", 40, 160, "crossover"),
                                        ("delay_ms", 0, 20, "delay")):
                    if k in b:
                        if need and not c[need]:
                            raise ValueError(f"{k} is not on this model")
                        sub[k] = clamp(num(b, k), lo, hi, float)
                for k, need in (("on", None), ("invert", "phase"), ("mains_hp", "mains_hp")):
                    if k in b:
                        if need and not c[need]:
                            raise ValueError(f"{k} is not on this model")
                        sub[k] = boolean(b, k)
            elif path == "zone":
                if caps["zones"] < 2 or b.get("zone") != "b":
                    raise ValueError("no such zone")
                z = a["zone_b"]
                if "on" in b:
                    z["on"] = boolean(b, "on")
                if "volume" in b:
                    z["volume"] = clamp(num(b, "volume"), 0, 100, int)
            elif path == "transport":
                cmd = b.get("cmd")
                if cmd not in ("play", "pause", "next", "prev"):
                    raise ValueError("cmd must be play, pause, next or prev")
                if a["source"] not in caps["transport"]:
                    raise ValueError("this source has no transport")
                s["playing"]["state"] = "pause" if cmd == "pause" else "play"
            elif path == "name":
                name = str(b.get("name", "")).strip()
                if not 1 <= len(name) <= 32:
                    raise ValueError("name must be 1 to 32 characters")
                s["device"]["name"] = name
            elif path == "amp":
                if b.get("clear_fault"):
                    s["power"]["amp_fault"] = False
                    s["power"]["amp_on"] = s["power"]["pdo_v"] >= 9
            elif path == "wifi":
                ssid = str(b.get("ssid", ""))
                if not ssid:
                    raise ValueError("ssid is required")
                ok = PASSWORDS.get(ssid) == b.get("password", "")
                s["net"]["join"] = ({"ssid": ssid, "state": "ok", "ip": "192.168.1.42"} if ok
                                    else {"ssid": ssid, "state": "failed", "reason": "wrong password"})
                if ok and s["net"]["mode"] == "sta":
                    s["net"].update(ssid=ssid, rssi=next((n["rssi"] for n in SCAN if n["ssid"] == ssid), -60))
            elif path == "wifi/forget":
                s["net"].update(mode="ap", ssid="", rssi=0, ip="192.168.4.1", join=None)
            elif path == "reboot":
                self.t0 = time.time() + 5 * 3600 + 17 * 60  # uptime starts again
            elif path == "reset":
                if b.get("factory") is not True:
                    raise ValueError("send {\"factory\": true} to erase the settings")
                d = s["device"]
                self.s = initial(model=d["model"], ap=True, pdo=(s["power"]["pdo_v"], s["power"]["pdo_a"]),
                                 fault=False)
            elif path == "update":
                if not str(b.get("url", "")).startswith(("http://", "https://")):
                    raise ValueError("url must be http(s)")
                s["update"] = {"state": "downloading", "progress": 0}
                threading.Thread(target=self._fake_update, daemon=True).start()
            else:
                raise KeyError(path)
            return copy.deepcopy(self.s)

    def selftest(self) -> dict:
        p = self.s["power"]
        items = [
            {"name": "USB-C power contract", "ok": p["pdo_v"] >= 9, "value": f"{p['pdo_v']} V × {p['pdo_a']} A"},
            {"name": "Supply (PVDD)", "ok": p["pdo_v"] >= 9, "value": f"{p['vbus_v']:.1f} V"},
            {"name": "Codec on I²C", "ok": True, "value": "0x10"},
            {"name": "Amplifier", "ok": not p["amp_fault"], "value": "fault" if p["amp_fault"] else "ready"},
            {"name": "Wi-Fi", "ok": self.s["net"]["mode"] == "sta", "value": f"{self.s['net']['rssi']} dBm"},
            {"name": "PSRAM", "ok": True, "value": "8 MB"},
        ]
        return {"ok": all(i["ok"] for i in items), "items": items}

    def _fake_update(self):
        for st, step in (("downloading", 20), ("flashing", 25)):
            for pct in range(0, 101, step):
                with self.lock:
                    self.s["update"] = {"state": st, "progress": pct}
                time.sleep(0.3)
        with self.lock:
            self.s["update"] = {"state": "done", "progress": 100}
            self.s["device"]["fw"] = "0.2.0"


def num(b, k):
    v = b.get(k)
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise ValueError(f"{k} must be a number")
    return v


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
            return p[len("/api/"):] if p.startswith("/api/") else None

        def do_GET(self):
            p = self.api()
            if p is None:
                return super().do_GET()
            if p == "state":
                return self.send(200, rx.state())
            if p == "wifi/scan":
                return self.send(200, SCAN)
            if p == "update/check":
                return self.send(200, {"current": rx.s["device"]["fw"], "latest": "0.2.0",
                                       "url": "https://example.invalid/hifi-lite-0.2.0.bin",
                                       "notes": "Faster Wi-Fi reconnect."})
            self.send(404, {"error": "no such endpoint"})

        def do_POST(self):
            p = self.api()
            n = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(n)
            if p == "update/upload":
                if n < 1024:
                    return self.send(400, {"error": "that file is too small to be firmware"})
                with rx.lock:
                    rx.s["update"] = {"state": "flashing", "progress": 0}
                threading.Thread(target=rx._fake_update, daemon=True).start()
                return self.send(200, rx.state())
            try:
                body = json.loads(raw or b"{}")
                if not isinstance(body, dict):
                    raise ValueError("body must be a JSON object")
                if p == "selftest":
                    return self.send(200, rx.selftest())
                return self.send(200, rx.post(p, body))
            except KeyError:
                self.send(404, {"error": "no such endpoint"})
            except (ValueError, json.JSONDecodeError) as e:
                self.send(400, {"error": str(e)})
    return H


def serve(port=8080, **kw) -> ThreadingHTTPServer:
    srv = ThreadingHTTPServer(("127.0.0.1", port), handler(Receiver(**kw)))
    srv.daemon_threads = True
    return srv


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--model", choices=sorted(CAPS), default="lite")
    ap.add_argument("--ap", action="store_true", help="start in first-run Wi-Fi setup")
    ap.add_argument("--pdo", default="20x5", help="charger contract, volts x amps (default 20x5)")
    ap.add_argument("--fault", action="store_true", help="start with an amplifier fault")
    o = ap.parse_args()
    v, a = (float(x) for x in o.pdo.lower().split("x"))
    srv = serve(o.port, model=o.model, ap=o.ap, pdo=(v, a), fault=o.fault)
    print(f"mock {o.model} receiver on http://127.0.0.1:{o.port}/", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
