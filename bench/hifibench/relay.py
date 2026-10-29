"""The relay: a board left at home, reached through an MQTT broker both sides
dial out to (docs/remote.md). `Relay` is a transport like api.Http and
api.Console, so every bench call works over it; commands are signed with the
Hive's Ed25519 key.

`Gate` is the board's side of the checks (signature, device id, replay,
clock); the mock board uses it, and the firmware does the same in C.
"""
from __future__ import annotations

import base64
import binascii
import hashlib
import json
import os
import queue
import threading
import time
from pathlib import Path

from . import API, ed25519, mqtt
from .api import ApiError

DEFAULT_CONFIG = Path("~/.config/hifi-receiver/relay.json")
TS_WINDOW_S = 600   # a command more than 10 minutes off the board's clock is refused
CHUNK = 4096        # ota/chunk's largest piece


# ---- config and keys ---------------------------------------------------------

def load_config(path: str | os.PathLike | None = None) -> dict:
    """relay.json: url, user, password, prefix, key (file with the seed in hex),
    and optionally board_user, board_password, cafile."""
    p = Path(path or os.environ.get("HIFI_RELAY") or DEFAULT_CONFIG).expanduser()
    if not p.is_file():
        raise FileNotFoundError(f"{p}: no relay settings (docs/remote.md, 'The Hive's side')")
    cfg = json.loads(p.read_text())
    cfg.setdefault("prefix", "hifi")
    if cfg.get("key"):
        k = Path(cfg["key"]).expanduser()
        cfg["key"] = str(k if k.is_absolute() else (p.parent / k))
    return cfg


def read_seed(path: str) -> bytes:
    seed = bytes.fromhex(Path(path).expanduser().read_text().strip())
    if len(seed) != 32:
        raise ValueError(f"{path}: an Ed25519 seed is 32 bytes (64 hex digits)")
    return seed


def write_seed(path: str) -> bytes:
    """A new signing key at path (0600); returns its public key."""
    p = Path(path).expanduser()
    p.parent.mkdir(parents=True, exist_ok=True)
    seed, pub = ed25519.keygen()
    fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(seed.hex() + "\n")
    return pub


def dev_id(mac: str) -> str:
    """The board's id on the relay: its MAC in lower-case hex, no colons."""
    return mac.replace(":", "").replace("-", "").lower()


# ---- the envelope ------------------------------------------------------------

def envelope(dev: str, cid: int, method: str, path: str, body: dict | None, ts: int | None = None) -> bytes:
    """Line 1 of a command: compact JSON in this key order."""
    e = {"v": 1, "dev": dev, "id": cid, "ts": int(time.time()) if ts is None else ts,
         "method": method, "path": path, "body": body}
    return json.dumps(e, separators=(",", ":")).encode()


def sign_command(seed: bytes, line: bytes) -> bytes:
    return line + b"\n" + base64.b64encode(ed25519.sign(seed, line))


class Gate:
    """What a board checks before it acts on a command."""

    def __init__(self, dev: str, trusted: list[bytes], clock=time.time):
        self.dev, self.trusted, self.clock = dev, trusted, clock
        self.last_id = 0
        self.clock_known = True

    def check(self, payload: bytes) -> tuple[int, dict]:
        """(200, envelope) or (status, {"id", "error"}): 400 unreadable, 401 bad
        signature or not for this board, 409 an old id or a clock far off."""
        line, _, sig = payload.partition(b"\n")
        try:
            e = json.loads(line)
            if not isinstance(e, dict):
                raise ValueError
        except ValueError:
            return 400, {"id": None, "error": "not a command"}
        cid = e.get("id")
        try:
            s = base64.b64decode(sig.strip(), validate=True)
        except (binascii.Error, ValueError):
            s = b""
        if not any(ed25519.verify(k, line, s) for k in self.trusted):
            return 401, {"id": cid, "error": "bad signature"}
        if e.get("v") != 1 or not isinstance(cid, int) or not isinstance(e.get("path"), str):
            return 400, {"id": cid, "error": "not a v1 command"}
        if e.get("dev") != self.dev:
            return 401, {"id": cid, "error": "not for this board"}
        if cid <= self.last_id:
            return 409, {"id": cid, "error": f"stale id (last {self.last_id})"}
        if self.clock_known and abs(int(e.get("ts", 0)) - self.clock()) > TS_WINDOW_S:
            return 409, {"id": cid, "error": "ts too far from the board's clock"}
        self.last_id = cid
        return 200, e


# ---- the Hive's side ---------------------------------------------------------

class Link:
    """One connection to the broker for the Hive: replies, events and status."""

    def __init__(self, cfg: dict, client_id: str | None = None):
        self.cfg = cfg
        self.prefix = cfg.get("prefix", "hifi")
        self.seed = read_seed(cfg["key"]) if cfg.get("key") else None
        self.status: dict[str, dict] = {}
        self.events: dict[str, queue.Queue] = {}
        self._waiting: dict[int, list] = {}
        self._lock = threading.Lock()
        self._last_id = 0
        self.c = mqtt.Client(cfg["url"], client_id or f"hive-{os.getpid()}-{int(time.time() * 1000) % 100000}",
                             cfg.get("user"), cfg.get("password"), cafile=cfg.get("cafile"))
        self.c.on_message = self._on
        self.c.connect()
        for t in ("status", "reply", "event"):
            self.c.subscribe(f"{self.prefix}/+/{t}", 1)

    def close(self):
        self.c.close()

    def _on(self, m: mqtt.Message):
        parts = m.topic.split("/")
        if len(parts) != 3 or parts[0] != self.prefix:
            return
        dev, kind = parts[1], parts[2]
        try:
            body = json.loads(m.payload) if m.payload else {}
        except ValueError:
            return
        if kind == "status":
            self.status[dev] = body
        elif kind == "event":
            self.events.setdefault(dev, queue.Queue()).put(body)
        elif kind == "reply":
            with self._lock:
                w = self._waiting.get(body.get("id"))
            if w:
                w[1] = body
                w[0].set()

    def next_id(self) -> int:
        with self._lock:
            self._last_id = max(int(time.time() * 1000), self._last_id + 1)
            return self._last_id

    def boards(self, wait_s: float = 1.0) -> dict[str, dict]:
        """The boards with a retained status (the broker sends them on subscribe)."""
        time.sleep(wait_s)
        return dict(self.status)

    def command(self, dev: str, method: str, path: str, body: dict | None, timeout: float = 15.0) -> dict:
        if not self.seed:
            raise ValueError("relay.json has no signing key ('key')")
        cid = self.next_id()
        w = [threading.Event(), None]
        with self._lock:
            self._waiting[cid] = w
        try:
            self.c.publish(f"{self.prefix}/{dev}/cmd", sign_command(self.seed, envelope(dev, cid, method, path, body)), 1)
            if not w[0].wait(timeout):
                raise ApiError(path, 0, f"no reply from {dev} in {timeout:g} s (is it online?)")
            return w[1]
        finally:
            with self._lock:
                self._waiting.pop(cid, None)

    def event(self, dev: str, kinds: tuple[str, ...], timeout: float) -> dict | None:
        q = self.events.setdefault(dev, queue.Queue())
        end = time.monotonic() + timeout
        while (left := end - time.monotonic()) > 0:
            try:
                e = q.get(timeout=left)
            except queue.Empty:
                return None
            if e.get("event") in kinds:
                return e
        return None


class Relay:
    """A transport (call(op, body, post)) to one board through a Link."""

    def __init__(self, link: Link, dev: str, timeout: float = 15.0):
        self.link, self.dev, self.timeout = link, dev, timeout

    def __str__(self):
        return f"relay {self.link.cfg['url']} {self.dev}"

    def call(self, op: str, body: dict | None = None, post: bool = False) -> dict:
        method = "POST" if (body is not None or post) else "GET"
        r = self.link.command(self.dev, method, f"{API}/{op}", body if method == "POST" else None, self.timeout)
        st, out = r.get("status", 0), r.get("body") or {}
        if st != 200:
            raise ApiError(op, st, out.get("error", "?") if isinstance(out, dict) else str(out),
                           out if isinstance(out, dict) else {})
        return out

    def wait_event(self, kinds: tuple[str, ...], timeout: float) -> dict | None:
        return self.link.event(self.dev, kinds, timeout)


def ota_push(t, image: bytes, version: str, timeout: float = 180.0, say=print) -> dict:
    """Send an image with ota/begin, ota/chunk and ota/finish, then wait for the
    trial boot's verdict. Over the relay that is the board's ota_valid or
    rolled_back event; elsewhere, GET ota once it answers again."""
    sha = hashlib.sha256(image).hexdigest()
    t.call("ota/begin", {"size": len(image), "sha256": sha, "version": version}, post=True)
    off, last_say = 0, 0.0
    while off < len(image):
        piece = image[off:off + CHUNK]
        try:
            r = t.call("ota/chunk", {"off": off, "data": base64.b64encode(piece).decode()}, post=True)
            off = int(r.get("next", off + len(piece)))
        except ApiError as e:
            if e.status == 409 and "next" in e.body:
                off = int(e.body["next"])  # the board says where it is: go on from there
                continue
            raise
        if time.monotonic() - last_say > 5:
            say(f"  ota: {off}/{len(image)} bytes")
            last_say = time.monotonic()
    t.call("ota/finish", {"reboot": True}, post=True)
    say(f"  ota: {len(image)} bytes sent, sha256 {sha[:12]}; the board restarts on trial")
    if hasattr(t, "wait_event"):
        e = t.wait_event(("ota_valid", "rolled_back", "ota_failed"), timeout)
        if e is None:
            return {"result": "timeout", "detail": f"no ota_valid or rolled_back in {timeout:g} s"}
        return {"result": "valid" if e["event"] == "ota_valid" else e["event"], "event": e}
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        time.sleep(2)
        try:
            st = t.call("ota")
        except (ApiError, OSError):
            continue
        if st.get("state") == "valid":
            ok = st.get("fw") == version
            return {"result": "valid" if ok else "rolled_back", "status": st}
    return {"result": "timeout"}
