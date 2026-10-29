"""A simulated board behind the same test API, so the bench runs with no board.

The model is coarse on purpose, but every number comes from product.md:
- PD: the CH224Q takes the highest PDO up to the board's PVDD (20 V on Lite);
  a laptop port is a 5 V-only source, and the amp refuses to start on it.
- Amp: bridge-tied, clipping at PVDD * R / (R + Rs); the input power is the
  output over 88% efficiency plus 0.6 W idle, through 0.15 ohm of cable and
  switch, so PVDD sags with power. Above the contract's watts the charger
  cuts out and the amp drops (FAULTZ low).
- Governor: holds input power at 90% of the contract by turning the gain
  down (`governor.active`, `limit_db`).
- Die temperature: 25 C + 22 C/W of dissipation, first order with a 40 s time
  constant, on the sim's own clock (`advance()` lets the soak's minutes pass at once). Lite has no sensor, so the
  API reports temp_c null and the mock's probe reads it at /mock/die_temp.

Served as HTTP (`serve`) and as the serial console on a pty (`serve_pty`).
`/mock/load`, `/mock/pd` and `/mock/fault` set the dummy load, the charger
and injected faults.
"""
from __future__ import annotations

import json
import math
import os
import random
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import API, predict
from .capture import Capture

CHARGERS = {
    "laptop": [[5, 0.9]],
    "65w": [[5, 3], [9, 3], [15, 3], [20, 3.25]],
    "100w": [[5, 3], [9, 3], [15, 3], [20, 5]],
    "140w": [[5, 3], [9, 3], [15, 3], [20, 5], [28, 5]],
}
EFF, IDLE_W, R_SRC = 0.88, 0.6, 0.15
RTH, TAU = 22.0, 40.0


class Sim:
    def __init__(self, board: str = "lite-main", charger: str = "100w", time_scale: float = 1.0, seed: int = 1):
        self.board = board
        self.tier = predict.TIERS[board]
        self.lock = threading.RLock()
        self.time_scale = time_scale
        self.rng = random.Random(seed)
        self.faults: set[str] = set()
        self.load = 8.0
        self.amp_on = False
        self.faultz = True
        self.volume_db = -30.0
        self.gov_on = True
        self.tone_args: dict | None = None
        self.ppm = 80.0  # the board's clock against the interface's
        self.temp = 25.0
        self._skip = 0.0                  # virtual seconds added by advance()
        self._t = self.now()
        self.events: list[str] = []
        self.set_charger(charger)

    # ---- the physics -----------------------------------------------------
    def set_charger(self, name: str):
        with self.lock:
            self.charger = name
            self.pdos = CHARGERS[name]
            ok = [p for p in self.pdos if p[0] <= self.tier["pvdd"]]
            self.contract = max(ok, key=lambda p: p[0])
            if self.contract[0] < 9:
                self.amp_on = False

    @property
    def source(self) -> str:
        return "5v" if len(self.pdos) == 1 else "pd"

    def _channels(self) -> int:
        if not self.tone_args:
            return 0
        return 2 if self.tone_args["channel"] in ("both", "sub") else 1

    def _ideal_vpk(self) -> float:
        if not (self.amp_on and self.tone_args):
            return 0.0
        fs_vpk = self.tier["pvdd"] * 1.05  # gain set so 0 dBFS just clips
        return fs_vpk * 10 ** ((self.tone_args["level_dbfs"] + self.volume_db) / 20)

    def operating_point(self) -> dict:
        """Solve PVDD, output, input power and governor for the present state."""
        with self.lock:
            v0, amax = self.contract
            pvdd, gain = float(v0), 1.0
            vpk = self._ideal_vpk()
            n = self._channels()
            limit_w = 0.9 * v0 * amax
            p_in = IDLE_W if self.amp_on else 0.25
            for _ in range(6):
                vclip = pvdd * self.load / (self.load + self.tier["rs"]) / 1.015
                out_pk = min(vpk * gain, vclip)
                p_out = n * out_pk * out_pk / (2 * self.load)
                p_in = p_out / EFF + (IDLE_W if self.amp_on else 0.25)
                if self.gov_on and p_in > limit_w and vpk > 0:
                    gain *= math.sqrt(max(limit_w - IDLE_W, 0.1) / max(p_in - IDLE_W, 0.1))
                pvdd = v0 - (p_in / pvdd) * R_SRC
            return {"pvdd": pvdd, "gain": gain, "vpk": vpk * gain, "vclip": vclip, "p_out": p_out,
                    "p_in": p_in, "limit_w": limit_w, "gov_active": gain < 0.999,
                    "trip": p_in > v0 * amax * 1.05}

    def now(self) -> float:
        """The sim's clock: real time (times time_scale) plus advance()."""
        return time.monotonic() * self.time_scale + self._skip

    def advance(self, seconds: float):
        """Let virtual time pass at once (the soak's sleep in the mock)."""
        with self.lock:
            step = 1.0
            while seconds > 0:
                self._skip += min(step, seconds)
                seconds -= step
                self.tick()

    def tick(self):
        """Advance the thermal model and the charger's over-current trip."""
        with self.lock:
            now = self.now()
            dt = now - self._t
            self._t = now
            op = self.operating_point()
            if op["trip"] and self.amp_on:
                self.amp_on, self.faultz = False, False
                self.events.append(f"charger cut out at {op['p_in']:.0f} W in")
            p_diss = op["p_out"] * (1 / EFF - 1) + (0.5 if self.amp_on else 0)
            target = 25 + RTH * p_diss
            self.temp += (target - self.temp) * (1 - math.exp(-dt / TAU))
            return op

    def waveform(self, n: int, rate: float, volts_per_fs: float) -> list[list[float]]:
        """Two inputs, leg+ and leg- against ground, AC-coupled, in full scale."""
        op = self.tick()
        f = (self.tone_args or {}).get("freq_hz", 1000.0) * (1 + self.ppm * 1e-6)
        ph = self.rng.random() * 2 * math.pi
        vpk, vclip = op["vpk"], op["vclip"]
        a, b = [], []
        g = self.rng.gauss
        for i in range(n):
            s = math.sin(2 * math.pi * f * i / rate + ph)
            v = vpk * s + 1e-4 * vpk * math.sin(3 * (2 * math.pi * f * i / rate + ph))
            v = max(-vclip, min(vclip, v))
            # Each leg carries half the bridge output; the divider scales it.
            a.append((v / 2) / volts_per_fs + g(0, 2e-6))
            b.append((-v / 2) / volts_per_fs + g(0, 2e-6))
        return [a, b]

    # ---- the API ---------------------------------------------------------
    def call(self, op: str, body: dict | None) -> tuple[int, dict]:
        body = body or {}
        with self.lock:
            st = self.tick()
            if op == "info":
                return 200, {"board": self.board, "fw_version": "mock-0.1", "idf": "v5.1",
                             "mac": "24:0a:c4:00:00:01", "hostname": "hifi-mock", "uptime_s": 12}
            if op == "state":
                v = st["pvdd"] if self.amp_on else self.contract[0]
                return 200, {
                    "pd": {"source": self.source, "contract_v": self.contract[0],
                           "contract_a": self.contract[1], "pdos": self.pdos},
                    "vbus_v": round(v, 2), "pvdd_v": round(v if self.amp_on else 0.0, 2),
                    "amp": {"enabled": self.amp_on, "faultz": self.faultz and "amp_fault" not in self.faults,
                            "muted": False},
                    "volume_db": self.volume_db, "input": "test",
                    "temp_c": None,
                    "governor": {"enabled": self.gov_on, "active": st["gov_active"],
                                 "limit_db": round(20 * math.log10(st["gain"]), 2)},
                    "net": {"wifi_rssi": -52, "ip": "127.0.0.1", "eth": False}}
            if op == "amp":
                if body.get("enable"):
                    if self.source == "5v" or self.contract[0] < 9:
                        return 409, {"error": "amp stays off on a 5 V source"}
                    self.amp_on, self.faultz = True, True
                else:
                    self.amp_on = False
                return 200, {"enabled": self.amp_on}
            if op == "volume":
                self.volume_db = max(-100.0, min(0.0, float(body["db"])))
                return 200, {"volume_db": self.volume_db}
            if op == "governor":
                self.gov_on = bool(body["enabled"])
                return 200, {"enabled": self.gov_on}
            if op == "tone":
                if body.get("channel", "both") not in ("L", "R", "both", "sub"):
                    return 400, {"error": "channel is L, R, both or sub"}
                self.tone_args = {"freq_hz": float(body["freq_hz"]), "level_dbfs": float(body["level_dbfs"]),
                                  "channel": body.get("channel", "both")}
                return 200, {"playing": True}
            if op == "tone/stop":
                self.tone_args = None
                return 200, {"playing": False}
            if op == "sweep":
                return 200, {"playing": True, "duration_s": body.get("duration_s", 5)}
            if op == "wifi":
                self.ssid = body["ssid"]
                return 200, {"ssid": self.ssid, "ip": "127.0.0.1"}
            if op == "selftest":
                items = [
                    ("pd_contract", self.source == "pd" and self.contract[0] >= 15, self.contract[0], ">= 15 V"),
                    ("vbus_in_range", True, self.contract[0], "within 5% of contract"),
                    ("codec_i2c_ack", "codec" not in self.faults, "0x10", "ACK"),
                    ("amp_faultz_high", self.faultz and "amp_fault" not in self.faults, 1, "high"),
                    ("amp_off_on_5v", "amp_on_5v" not in self.faults, 0, "PVDD_EN low on 5 V"),
                    ("wifi_or_eth_up", True, -52, "linked"),
                    ("psram", True, 8, "8 MB"),
                ]
                # On a laptop port the PD item is expected to fail; it isn't a fault.
                out = [{"name": n, "pass": bool(p), "value": v, "limit": l, "detail": ""} for n, p, v, l in items]
                return 200, {"pass": all(i["pass"] for i in out), "items": out}
            if op == "mock/load":
                self.load = float(body["ohm"])
                return 200, {"ohm": self.load}
            if op == "mock/pd":
                self.set_charger(body["charger"])
                return 200, {"contract": self.contract}
            if op == "mock/fault":
                self.faults = set(body.get("faults", []))
                return 200, {"faults": sorted(self.faults)}
            if op == "mock/die_temp":
                return 200, {"temp_c": round(self.temp, 2)}
            return 404, {"error": f"no op {op}"}

    def console_line(self, line: str) -> str | None:
        line = line.strip()
        if not line.startswith("api "):
            return None
        op, _, js = line[4:].partition(" ")
        try:
            code, out = self.call(op, json.loads(js) if js else None)
        except (ValueError, KeyError) as e:
            code, out = 400, {"error": str(e)}
        if code >= 400:
            out = {"error": out.get("error", "?"), "status": code}
        return "API " + json.dumps(out)


class MockCapture(Capture):
    def __init__(self, sim: Sim, volts_per_fs: float, rate: int = 48000):
        self.sim, self.volts_per_fs, self.rate = sim, volts_per_fs, rate

    def record(self, seconds):
        return self.sim.waveform(int(seconds * self.rate), self.rate, self.volts_per_fs)


def serve(sim: Sim, host: str = "127.0.0.1", port: int = 0) -> ThreadingHTTPServer:
    """Start the HTTP API in a thread; the server's port is .server_port."""

    class H(BaseHTTPRequestHandler):
        def _go(self, body):
            path = self.path.split("?")[0]
            if path.startswith(API + "/"):
                op = path[len(API) + 1:]
            elif path.startswith("/mock/"):
                op = path[1:]
            else:
                op = ""
            code, out = sim.call(op, body)
            data = json.dumps(out).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            self._go(None)

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            try:
                body = json.loads(self.rfile.read(n) or b"{}")
            except ValueError:
                body = {}
            self._go(body)

        def log_message(self, *a):
            pass

    srv = ThreadingHTTPServer((host, port), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def serve_pty(sim: Sim) -> str:
    """The serial console on a pseudo-terminal; returns the slave's path."""
    import pty
    import tty
    master, slave = pty.openpty()
    tty.setraw(slave)
    path = os.ttyname(slave)

    def loop():
        buf = b""
        os.write(master, b"I (312) boot: mock console\n")
        while True:
            try:
                chunk = os.read(master, 1024)
            except OSError:
                return
            buf += chunk
            while b"\n" in buf:
                line, _, buf = buf.partition(b"\n")
                ans = sim.console_line(line.decode(errors="replace"))
                if ans:
                    os.write(master, b"I (999) log: noise before the answer\n" + ans.encode() + b"\n")

    threading.Thread(target=loop, daemon=True).start()
    serve_pty.keep = getattr(serve_pty, "keep", []) + [slave]  # keep the slave open
    return path
