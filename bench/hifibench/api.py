"""The board's test API (docs/test-api.md), over HTTP or the USB serial console.

Both transports carry the same ops and the same JSON: HTTP as
`GET|POST /api/v1/<op>`, the console as a line `api <op> [json]` answered by
one line `API {json}` (an error as `API {"error": ..., "status": N}`). So a
board on a laptop's USB port with no network is tested the same way as one
found on the LAN.
"""
from __future__ import annotations

import json
import time
import urllib.error
import urllib.request

from . import API


class ApiError(Exception):
    def __init__(self, op: str, status: int, msg: str):
        super().__init__(f"{op}: {status} {msg}")
        self.op, self.status, self.msg = op, status, msg


class Http:
    # A board is on the bench's own LAN: never through an HTTP(S)_PROXY.
    _open = urllib.request.build_opener(urllib.request.ProxyHandler({})).open

    def __init__(self, base: str, timeout: float = 10.0):
        self.base = base.rstrip("/")
        self.timeout = timeout

    def __str__(self):
        return self.base

    def call(self, op: str, body: dict | None = None, post: bool = False) -> dict:
        data = None if body is None and not post else json.dumps(body or {}).encode()
        req = urllib.request.Request(self.base + API + "/" + op, data=data,
                                     headers={"Content-Type": "application/json"})
        try:
            with self._open(req, timeout=self.timeout) as r:
                return json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            try:
                msg = json.loads(e.read()).get("error", e.reason)
            except Exception:
                msg = e.reason
            raise ApiError(op, e.code, str(msg)) from None


class Console:
    """The API over a line-based stream (a serial port, or a mock's pipe)."""

    def __init__(self, port, timeout: float = 10.0, name: str = "serial"):
        self.port, self.timeout, self.name = port, timeout, name

    def __str__(self):
        return self.name

    def call(self, op: str, body: dict | None = None, post: bool = False) -> dict:
        line = f"api {op}" + (" " + json.dumps(body) if body is not None else "")
        self.port.write((line + "\n").encode())
        end = time.monotonic() + self.timeout
        while time.monotonic() < end:
            raw = self.port.readline()
            if not raw:
                continue
            text = raw.decode(errors="replace").strip()
            if text.startswith("API "):  # everything else is the firmware's log
                out = json.loads(text[4:])
                if isinstance(out, dict) and "error" in out:
                    raise ApiError(op, out.get("status", 400), out["error"])
                return out
        raise ApiError(op, 0, f"no answer on {self.name} in {self.timeout:g} s")


class Device:
    """One board under test."""

    def __init__(self, transport):
        self.t = transport

    def __str__(self):
        return str(self.t)

    def info(self) -> dict:
        return self.t.call("info")

    def state(self) -> dict:
        return self.t.call("state")

    def amp(self, enable: bool) -> dict:
        return self.t.call("amp", {"enable": enable}, post=True)

    def volume(self, db: float) -> dict:
        return self.t.call("volume", {"db": db}, post=True)

    def governor(self, enabled: bool) -> dict:
        return self.t.call("governor", {"enabled": enabled}, post=True)

    def tone(self, freq_hz: float, level_dbfs: float, channel: str = "both", duration_s: float = 0) -> dict:
        return self.t.call("tone", {"freq_hz": freq_hz, "level_dbfs": level_dbfs,
                                    "channel": channel, "duration_s": duration_s}, post=True)

    def tone_stop(self) -> dict:
        return self.t.call("tone/stop", {}, post=True)

    def sweep(self, start_hz: float, stop_hz: float, level_dbfs: float, duration_s: float) -> dict:
        return self.t.call("sweep", {"start_hz": start_hz, "stop_hz": stop_hz,
                                     "level_dbfs": level_dbfs, "duration_s": duration_s}, post=True)

    def wifi(self, ssid: str, password: str) -> dict:
        """Join a network (over the console, before the charger takes the USB port)."""
        return self.t.call("wifi", {"ssid": ssid, "password": password}, post=True)

    def selftest(self) -> dict:
        return self.t.call("selftest", {}, post=True)
