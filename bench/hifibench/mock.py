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

Served as HTTP (`serve`), as the serial console on a pty (`serve_pty`) and
on a relay broker (`RelayBoard`, docs/remote.md). `/mock/load`, `/mock/pd`
and `/mock/fault` set the dummy load, the charger and injected faults.

The firmware's own calls are modelled too: tones in its shape (`"type":
"sine"`, at unity volume), `capture` from the loopback taps (one leg of each
bridge-tied output through the 1/11 divider), the OTA calls with a trial
boot (an image containing `HIFI-BAD-IMAGE` fails it and rolls back),
`wifi` and `remote`.
"""
from __future__ import annotations

import base64
import hashlib
import json
import math
import os
import queue
import random
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import API, predict, relay
from .api import split_argv
from .capture import Capture

CHARGERS = {
    "laptop": [[5, 0.9]],
    "65w": [[5, 3], [9, 3], [15, 3], [20, 3.25]],
    "100w": [[5, 3], [9, 3], [15, 3], [20, 5]],
    "140w": [[5, 3], [9, 3], [15, 3], [20, 5], [28, 5]],
}
EFF, IDLE_W, R_SRC = 0.88, 0.6, 0.15
RTH, TAU = 22.0, 40.0
# The loopback taps (docs/remote.md): speaker-leg peak volts per ADC full scale.
TAP_V_PER_FS = {"lite-main": 1.27 * 11, "core": 1.27 * 16}
OTA_SLOT_MAX = 0x330000
BAD_IMAGE = b"HIFI-BAD-IMAGE"


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
        self.mac = "24:0a:c4:00:00:01"
        self.ip: str | None = "127.0.0.1"
        self.ssid = "bench"
        self.fw, self.slot, self.ota_state = "mock-0.1", "ota_0", "valid"
        self.update: dict | None = None
        self.ota_buf = bytearray()
        self.pending_boot: dict | None = None
        self.last_rollback: dict | None = None
        self.reboot_hook = None   # set by a RelayBoard: it restarts the connection around boot()
        self.remote_cfg: dict | None = None
        self.remote_hook = None   # called with the settings `remote` is given
        self.remote_connected = lambda: False
        self.boots = 0
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
        vol = self.volume_db
        if self.tone_args.get("unity"):   # the firmware's tones: unity volume, its own gain figure
            fs_vpk, vol = self.tier.get("fs_vpk") or fs_vpk, 0.0
        return fs_vpk * 10 ** ((self.tone_args["level_dbfs"] + vol) / 20)

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

    # ---- what the firmware adds ---------------------------------------------
    def capture(self, body: dict) -> tuple[int, dict]:
        """POST capture: what the ADC records from the taps (or the RCA input)."""
        src = body.get("source", "loopback")
        n = int(body.get("frames", 4096))
        if src not in ("loopback", "line"):
            return 400, {"error": "source must be loopback or line"}
        if not 64 <= n <= 16384:
            return 400, {"error": "frames must be 64 to 16384"}
        if not self.tone_args:
            return 409, {"error": "output idle: play a tone or a stream first"}
        op = self.tick()
        rate, vpf = 48000, TAP_V_PER_FS.get(self.board, 14.0)
        f = self.tone_args.get("freq_hz", 1000.0) * (1 + self.ppm * 1e-6)
        lc = 1 / math.sqrt(1 + (f / 40000) ** 4)   # the output filter, about 40 kHz
        vpk, vclip = op["vpk"] * lc, op["vclip"]
        drive = {"both": (1, 1), "sub": (1, 1), "L": (1, 0), "R": (0, 1)}[self.tone_args.get("channel", "both")]
        if "amp_r_dead" in self.faults:
            drive = (drive[0], 0)  # the right channel's stage puts nothing out
        if src == "line":
            drive = (0, 0)  # nothing plugged into the RCA input
        g, ph = self.rng.gauss, self.rng.random() * 2 * math.pi
        pcm, stats = [], [{"sq": 0.0, "pk": 0.0, "sum": 0.0, "clip": 0} for _ in range(2)]
        for i in range(n):
            w = 2 * math.pi * f * i / rate + ph
            for c in range(2):
                v = drive[c] * (vpk * math.sin(w) + 1e-4 * vpk * math.sin(3 * w))
                v = max(-vclip, min(vclip, v))
                x = (v / 2) / vpf + g(0, 2e-5)     # one leg: half the bridge's output; ADC noise about -94 dBFS
                q = max(-32768, min(32767, round(x * 32768)))
                st = stats[c]
                st["clip"] += q in (-32768, 32767)
                xf = q / 32768
                st["sq"] += xf * xf
                st["sum"] += xf
                st["pk"] = max(st["pk"], abs(xf))
                pcm.append(q)
        raw = struct.pack(f"<{len(pcm)}h", *pcm)
        names = ["spk_l+", "spk_r+"] if src == "loopback" else ["line_l", "line_r"]
        return 200, {"source": src, "rate": rate, "frames": n, "channels": names,
                     "v_per_fs": [vpf, vpf] if src == "loopback" else [None, None],
                     "stats": [{"rms_fs": round(math.sqrt(x["sq"] / n), 7), "peak_fs": round(x["pk"], 6),
                                "dc_fs": round(x["sum"] / n, 7), "clipped": x["clip"]} for x in stats],
                     "pcm": base64.b64encode(raw).decode()}

    def _fw_tone(self, body: dict) -> tuple[int, dict]:
        """POST tone in the firmware's shape (docs/test-api.md)."""
        t = body["type"]
        if t == "stop":
            self.tone_args = None
            return 200, {"ok": True, "tone": None}
        if t not in ("sine", "sweep", "noise", "silence"):
            return 400, {"error": "type must be sine, sweep, noise, silence or stop"}
        level = float(body.get("level_dbfs", -20))
        force = bool(body.get("force"))
        if level > (0 if force else -6) or level < -90:
            return 400, {"error": "level_dbfs must be -90 to -6, or up to 0 with force"}
        ch = {"both": "both", "left": "L", "right": "R"}.get(body.get("channel", "both"))
        if not ch:
            return 400, {"error": "channel must be both, left or right"}
        if (self.source == "5v" or self.contract[0] < 9) and not force:
            return 409, {"error": "power isn't on (wait_pd)"}
        if self.source != "5v" and self.contract[0] >= 9:
            self.amp_on, self.faultz = True, True   # the firmware wakes the amp for audio
        self.tone_args = {"type": t, "freq_hz": float(body.get("freq_hz", 1000)),
                          "level_dbfs": -300.0 if t == "silence" else level, "channel": ch, "unity": True}
        return 200, {"ok": True, "tone": dict(body)}

    def ota_status(self) -> dict:
        u = self.update
        return {"slot": self.slot, "fw": self.fw, "state": self.ota_state,
                "update": None if not u else {"size": u["size"], "written": len(self.ota_buf), "version": u["version"]},
                "last_rollback": self.last_rollback}

    def _ota(self, op: str, body: dict) -> tuple[int, dict]:
        if op == "ota/begin":
            size, sha, ver = body.get("size"), str(body.get("sha256", "")), body.get("version")
            if not isinstance(size, int) or not 0 < size <= OTA_SLOT_MAX:
                return 400, {"error": f"size must be 1 to {OTA_SLOT_MAX} bytes"}
            if len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha) or not ver:
                return 400, {"error": "sha256 (64 hex digits) and version are required"}
            self.update, self.ota_buf = {"size": size, "sha256": sha, "version": str(ver)}, bytearray()
            return 200, {"ok": True, "slot": "ota_1" if self.slot == "ota_0" else "ota_0", "chunk_max": relay.CHUNK}
        if op == "ota/abort":
            self.update, self.ota_buf = None, bytearray()
            return 200, {"ok": True}
        if not self.update:
            return 409, {"error": "no update open (ota/begin first)"}
        if op == "ota/chunk":
            off = body.get("off")
            if off != len(self.ota_buf):
                return 409, {"error": f"expected offset {len(self.ota_buf)}", "next": len(self.ota_buf)}
            try:
                data = base64.b64decode(body.get("data", ""), validate=True)
            except ValueError:
                return 400, {"error": "data isn't base64"}
            if not data or len(data) > relay.CHUNK or off + len(data) > self.update["size"]:
                return 400, {"error": "a chunk is 1 to 4096 bytes and stays inside size"}
            self.ota_buf += data
            return 200, {"ok": True, "next": len(self.ota_buf)}
        if op == "ota/finish":
            if len(self.ota_buf) != self.update["size"]:
                return 409, {"error": f"have {len(self.ota_buf)} of {self.update['size']} bytes",
                             "next": len(self.ota_buf)}
            if hashlib.sha256(self.ota_buf).hexdigest() != self.update["sha256"]:
                self.update, self.ota_buf = None, bytearray()
                return 400, {"error": "sha256 mismatch"}
            self.pending_boot = {"version": self.update["version"], "bad": BAD_IMAGE in self.ota_buf}
            self.update, self.ota_buf = None, bytearray()
            return 200, {"ok": True, "reboot": True}
        return 404, {"error": f"no op {op}"}

    def boot(self) -> dict:
        """The restart after ota/finish, and the new image's trial: the event it publishes."""
        with self.lock:
            p, self.pending_boot = self.pending_boot, None
            self.boots += 1
            self.tone_args, self.amp_on = None, False
            if not p:
                return {"event": "boot", "fw": self.fw}
            if p["bad"]:
                reason = "trial: no relay connection in 120 s"
                self.last_rollback = {"from": p["version"], "reason": reason}
                return {"event": "rolled_back", "from": p["version"], "to": self.fw, "reason": reason}
            self.fw, self.slot, self.ota_state = p["version"], ("ota_1" if self.slot == "ota_0" else "ota_0"), "valid"
            return {"event": "ota_valid", "fw": self.fw, "slot": self.slot}

    def remote_status(self) -> dict:
        c = self.remote_cfg or {}
        return {"configured": bool(c.get("url")), "url": c.get("url"), "prefix": c.get("prefix", "hifi"),
                "user": c.get("user"), "connected": bool(self.remote_connected()), "dev": relay.dev_id(self.mac)}

    # ---- the API ---------------------------------------------------------
    def call(self, op: str, body: dict | None) -> tuple[int, dict]:
        get = body is None
        body = body or {}
        if op == "ota/finish":
            code, out = self._ota(op, body)
            if code == 200 and not self.reboot_hook:
                self.boot()   # over HTTP or the console the reboot just happens
            return code, out
        if op == "remote" and not get:
            if not str(body.get("url", "")).startswith(("mqtt://", "mqtts://")):
                return 400, {"error": "url must be mqtt:// or mqtts://"}
            self.remote_cfg = {k: body.get(k) for k in ("url", "user", "password", "prefix")}
            self.remote_cfg["prefix"] = self.remote_cfg["prefix"] or "hifi"
            if self.remote_hook:
                self.remote_hook(self.remote_cfg)
            return 200, self.remote_status()
        with self.lock:
            st = self.tick()
            if op == "capture":
                return self.capture(body)
            if op == "ota" and get:
                return 200, self.ota_status()
            if op.startswith("ota/"):
                return self._ota(op, body)
            if op == "remote":
                return 200, self.remote_status()
            if op == "tone" and "type" in body:
                return self._fw_tone(body)
            if op == "info":
                return 200, {"board": self.board, "model": "hifi-lite" if self.board == "lite-main" else "hifi-plus",
                             "fw": self.fw, "fw_version": self.fw, "idf": "v4.4.8", "api": 1,
                             "mac": self.mac, "hostname": "hifi-mock", "uptime_s": 12}
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
                    "net": {"wifi_rssi": -52, "ip": self.ip, "ssid": self.ssid, "eth": False}}
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
                if not body.get("ssid"):
                    return 400, {"error": "ssid is required"}
                self.ssid, self.ip = body["ssid"], "127.0.0.1"
                return 200, {"ok": True, "ssid": self.ssid}
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
        argv = split_argv(line)
        if len(argv) < 2 or len(argv) > 3:
            return "API " + json.dumps({"error": 'usage: api <op> ["<json>"]', "status": 400})
        try:
            code, out = self.call(argv[1], json.loads(argv[2]) if len(argv) == 3 else None)
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


class RelayBoard:
    """The mock board on a broker, as the firmware is (docs/remote.md): a
    retained status with a last will, signed commands on <prefix>/<dev>/cmd
    checked by relay.Gate, replies and events. After ota/finish it drops the
    connection the way a restart does, boots, and comes back."""

    def __init__(self, sim: Sim, url: str, trusted: list[bytes], user: str | None = None,
                 password: str | None = None, prefix: str = "hifi"):
        self.sim, self.url, self.user, self.password, self.prefix = sim, url, user, password, prefix
        self.trusted = trusted
        self.dev = relay.dev_id(sim.mac)
        self.base = f"{prefix}/{self.dev}"
        self.c = None
        self.work: "queue.Queue" = queue.Queue()
        sim.reboot_hook = self._restart
        sim.remote_connected = lambda: bool(self.c and self.c.connected)
        threading.Thread(target=self._worker, daemon=True).start()
        self._connect({"event": "boot", "fw": sim.fw})

    def _connect(self, event: dict):
        from . import mqtt
        self.gate = relay.Gate(self.dev, self.trusted)   # RAM state: a restart clears the last id
        will = (self.base + "/status", json.dumps({"online": False}).encode(), 1, True)
        c = mqtt.Client(self.url, "board-" + self.dev, self.user, self.password, keepalive=10, will=will)
        c.on_message = lambda m: self.work.put(m)   # never publish from the reader thread
        c.connect()
        self.c = c
        c.publish(self.base + "/status", json.dumps(self.status()), 1, retain=True)
        c.subscribe(self.base + "/cmd", 1)
        c.publish(self.base + "/event", json.dumps(event), 1)

    def status(self) -> dict:
        s = self.sim
        return {"online": True, "model": "hifi-lite" if s.board == "lite-main" else "hifi-plus", "board": s.board,
                "fw": s.fw, "ip": s.ip, "rssi": -52, "uptime_s": 12, "ota": {"slot": s.slot, "state": s.ota_state}}

    def _worker(self):
        while True:
            m = self.work.get()
            if m is None:
                return
            try:
                self._handle(m.payload)
            except Exception as e:  # keep serving, as the firmware would
                print(f"mock relay board: {e!r}")

    def _handle(self, payload: bytes):
        st, e = self.gate.check(payload)
        if st != 200:
            self.c.publish(self.base + "/reply", json.dumps({"id": e.get("id"), "status": st,
                                                             "body": {"error": e["error"]}}), 1)
            return
        path = e["path"]
        op = path[len(API) + 1:] if path.startswith(API + "/") else ""
        code, out = self.sim.call(op, (e.get("body") or {}) if e.get("method") == "POST" else None)
        self.c.publish(self.base + "/reply", json.dumps({"id": e["id"], "status": code, "body": out}), 1)
        if self.sim.pending_boot:
            self._restart()

    def _restart(self):
        self.c.drop()          # no DISCONNECT: the broker sends the last will
        time.sleep(0.05)
        self._connect(self.sim.boot())

    def close(self):
        self.work.put(None)
        if self.c:
            self.c.close()


def relay_rig(board: str = "lite-main", charger: str = "100w", logins: dict | None = None, keydir: str | None = None):
    """A broker, a signing key and a mock board on it: (broker, sim, board, cfg).
    cfg is what relay.json would hold (key is a file in keydir)."""
    import tempfile
    from .broker import Broker
    br = Broker(logins=logins)
    keydir = keydir or tempfile.mkdtemp(prefix="hifi-relay-")
    key = os.path.join(keydir, "hive-signing.key")
    pub = relay.write_seed(key)
    cfg = {"url": br.url, "prefix": "hifi", "key": key}
    if logins:
        cfg.update(user="hive", password=logins.get("hive"), board_user="board", board_password=logins.get("board"))
    sim = Sim(board, charger=charger)
    rb = RelayBoard(sim, br.url, [pub], cfg.get("board_user"), cfg.get("board_password"))
    return br, sim, rb, cfg
