"""A small MQTT 3.1.1 client in plain Python, for the relay (docs/remote.md).

Enough for the bench: CONNECT with a login and a last will, SUBSCRIBE at
QoS 0/1, PUBLISH at QoS 0/1 (PUBACKs both ways), retained messages and the
keepalive. `mqtts://` is TLS, `mqtt://` plain. When HTTPS_PROXY (or
https_proxy) is set and the host isn't in NO_PROXY, the TCP connection goes
through the proxy with HTTP CONNECT: that is how an agent's sandbox, which
only reaches out through a proxy, gets to the broker.

The packet helpers are shared with the test broker (broker.py).
"""
from __future__ import annotations

import base64
import os
import queue
import socket
import ssl
import struct
import threading
import time
import urllib.parse
from dataclasses import dataclass

CONNECT, CONNACK, PUBLISH, PUBACK = 1, 2, 3, 4
SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK = 8, 9, 10, 11
PINGREQ, PINGRESP, DISCONNECT = 12, 13, 14


class MqttError(Exception):
    pass


# ---- packets -------------------------------------------------------------

def _varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b, n = n % 128, n // 128
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def s16(b: bytes | str) -> bytes:
    if isinstance(b, str):
        b = b.encode()
    return struct.pack("!H", len(b)) + b


def packet(kind: int, flags: int, body: bytes) -> bytes:
    return bytes([kind << 4 | flags]) + _varint(len(body)) + body


def _read_exact(sock, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf += chunk
    return buf


def read_packet(sock) -> tuple[int, int, bytes]:
    """(type, flags, body) of the next packet; raises ConnectionError at EOF."""
    h = _read_exact(sock, 1)[0]
    n, mult = 0, 1
    for _ in range(4):
        b = _read_exact(sock, 1)[0]
        n += (b & 0x7F) * mult
        if not b & 0x80:
            break
        mult *= 128
    else:
        raise MqttError("malformed length")
    return h >> 4, h & 0x0F, _read_exact(sock, n) if n else b""


def take_s16(b: bytes, i: int) -> tuple[bytes, int]:
    n = struct.unpack_from("!H", b, i)[0]
    return b[i + 2:i + 2 + n], i + 2 + n


def publish_packet(topic: str, payload: bytes, qos: int = 0, retain: bool = False, pid: int = 0,
                   dup: bool = False) -> bytes:
    body = s16(topic) + (struct.pack("!H", pid) if qos else b"") + payload
    return packet(PUBLISH, (dup << 3) | (qos << 1) | int(retain), body)


def parse_publish(flags: int, body: bytes) -> tuple[str, bytes, int, bool, int]:
    qos, retain = (flags >> 1) & 3, bool(flags & 1)
    t, i = take_s16(body, 0)
    pid = 0
    if qos:
        pid = struct.unpack_from("!H", body, i)[0]
        i += 2
    return t.decode(), body[i:], qos, retain, pid


def topic_matches(filt: str, topic: str) -> bool:
    f, t = filt.split("/"), topic.split("/")
    for i, part in enumerate(f):
        if part == "#":
            return True
        if i >= len(t):
            return False
        if part != "+" and part != t[i]:
            return False
    return len(f) == len(t)


# ---- the network path ------------------------------------------------------

def _no_proxy(host: str) -> bool:
    if host in ("localhost", "127.0.0.1", "::1"):
        return True
    for n in (os.environ.get("NO_PROXY") or os.environ.get("no_proxy") or "").split(","):
        n = n.strip().lstrip(".")
        if n and (host == n or host.endswith("." + n) or n == "*"):
            return True
    return False


def open_tcp(host: str, port: int, timeout: float = 15.0) -> socket.socket:
    """A TCP connection to host:port, through HTTPS_PROXY's CONNECT when set."""
    proxy = os.environ.get("HTTPS_PROXY") or os.environ.get("https_proxy")
    if not proxy or _no_proxy(host):
        return socket.create_connection((host, port), timeout=timeout)
    u = urllib.parse.urlsplit(proxy if "://" in proxy else "http://" + proxy)
    s = socket.create_connection((u.hostname, u.port or 80), timeout=timeout)
    req = f"CONNECT {host}:{port} HTTP/1.1\r\nHost: {host}:{port}\r\n"
    if u.username:
        cred = f"{urllib.parse.unquote(u.username)}:{urllib.parse.unquote(u.password or '')}"
        req += "Proxy-Authorization: Basic " + base64.b64encode(cred.encode()).decode() + "\r\n"
    s.sendall((req + "\r\n").encode())
    head = b""
    while b"\r\n\r\n" not in head:
        c = s.recv(1)
        if not c:
            raise MqttError("proxy closed the CONNECT")
        head += c
    status = head.split(b"\r\n", 1)[0].split()
    if len(status) < 2 or status[1] != b"200":
        s.close()
        raise MqttError(f"proxy refused CONNECT {host}:{port}: {head.splitlines()[0].decode(errors='replace')}")
    return s


@dataclass
class Message:
    topic: str
    payload: bytes
    qos: int = 0
    retain: bool = False


class Client:
    """One connection. Received messages go to `on_message(msg)` when set,
    else into `.messages` (a queue)."""

    def __init__(self, url: str, client_id: str, user: str | None = None, password: str | None = None,
                 keepalive: int = 30, will: tuple[str, bytes, int, bool] | None = None,
                 cafile: str | None = None, timeout: float = 15.0):
        u = urllib.parse.urlsplit(url)
        if u.scheme not in ("mqtt", "mqtts"):
            raise ValueError("the relay url is mqtt://host:port or mqtts://host:port")
        self.host, self.tls = u.hostname, u.scheme == "mqtts"
        self.port = u.port or (8883 if self.tls else 1883)
        self.client_id, self.user, self.password = client_id, user, password
        self.keepalive, self.will, self.cafile, self.timeout = keepalive, will, cafile, timeout
        self.messages: queue.Queue[Message] = queue.Queue()
        self.on_message = None
        self._sock = None
        self._wlock = threading.Lock()
        self._pid = 0
        self._acks: dict[int, threading.Event] = {}
        self._closed = threading.Event()
        self.connected = False

    # ---- connect -----------------------------------------------------------
    def connect(self) -> "Client":
        s = open_tcp(self.host, self.port, self.timeout)
        if self.tls:
            ctx = ssl.create_default_context(cafile=self.cafile)
            s = ctx.wrap_socket(s, server_hostname=self.host)
        s.settimeout(self.timeout)
        flags = 0x02  # clean session
        payload = s16(self.client_id)
        if self.will:
            t, p, q, r = self.will
            flags |= 0x04 | (q << 3) | (0x20 if r else 0)
            payload += s16(t) + s16(p)
        if self.user is not None:
            flags |= 0x80
            payload += s16(self.user)
            if self.password is not None:
                flags |= 0x40
                payload += s16(self.password)
        body = s16("MQTT") + bytes([4, flags]) + struct.pack("!H", self.keepalive) + payload
        s.sendall(packet(CONNECT, 0, body))
        kind, _, b = read_packet(s)
        if kind != CONNACK or len(b) < 2:
            raise MqttError("no CONNACK")
        if b[1]:
            reason = {1: "protocol", 2: "client id", 3: "server unavailable", 4: "bad login",
                      5: "not authorised"}.get(b[1], str(b[1]))
            raise MqttError(f"broker refused the connection: {reason}")
        s.settimeout(None)
        self._sock, self.connected = s, True
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._pinger, daemon=True).start()
        return self

    def _send(self, data: bytes):
        if not self._sock:
            raise MqttError("not connected")
        with self._wlock:
            self._sock.sendall(data)

    def _next_pid(self) -> int:
        with self._wlock:
            self._pid = self._pid % 65535 + 1
            return self._pid

    def _wait_ack(self, pid: int, ev: threading.Event, what: str):
        if not ev.wait(self.timeout):
            self._acks.pop(pid, None)
            raise MqttError(f"no {what} from the broker in {self.timeout:g} s")

    # ---- calls ---------------------------------------------------------------
    def subscribe(self, topic: str, qos: int = 1):
        pid = self._next_pid()
        ev = self._acks[pid] = threading.Event()
        self._send(packet(SUBSCRIBE, 2, struct.pack("!H", pid) + s16(topic) + bytes([qos])))
        self._wait_ack(pid, ev, "SUBACK")

    def publish(self, topic: str, payload: bytes | str, qos: int = 0, retain: bool = False):
        if isinstance(payload, str):
            payload = payload.encode()
        if not qos:
            self._send(publish_packet(topic, payload, 0, retain))
            return
        pid = self._next_pid()
        ev = self._acks[pid] = threading.Event()
        self._send(publish_packet(topic, payload, 1, retain, pid))
        self._wait_ack(pid, ev, "PUBACK")

    def close(self):
        """A clean DISCONNECT: the broker drops the last will."""
        if self._sock and self.connected:
            try:
                self._send(packet(DISCONNECT, 0, b""))
            except OSError:
                pass
        self._drop()

    def drop(self):
        """Close the socket without DISCONNECT, as a crash would: the will fires."""
        self._drop()

    def _drop(self):
        self.connected = False
        self._closed.set()
        if self._sock:
            try:
                self._sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self._sock.close()

    # ---- threads ---------------------------------------------------------------
    def _reader(self):
        try:
            while True:
                kind, flags, body = read_packet(self._sock)
                if kind == PUBLISH:
                    t, p, q, r, pid = parse_publish(flags, body)
                    if q == 1:
                        self._send(packet(PUBACK, 0, struct.pack("!H", pid)))
                    m = Message(t, p, q, r)
                    if self.on_message:
                        try:
                            self.on_message(m)
                        except Exception as e:  # a handler's bug mustn't kill the connection
                            print(f"mqtt: on_message failed: {e!r}")
                    else:
                        self.messages.put(m)
                elif kind in (PUBACK, SUBACK, UNSUBACK):
                    pid = struct.unpack_from("!H", body)[0]
                    ev = self._acks.pop(pid, None)
                    if ev:
                        ev.set()
        except (OSError, ConnectionError, MqttError):
            pass
        self.connected = False

    def _pinger(self):
        while not self._closed.wait(max(1.0, self.keepalive / 2)):
            try:
                self._send(packet(PINGREQ, 0, b""))
            except OSError:
                return


def wait_for(q: "queue.Queue[Message]", pred, timeout: float) -> Message | None:
    """The first message on q that pred accepts, within timeout; others are dropped."""
    end = time.monotonic() + timeout
    while True:
        left = end - time.monotonic()
        if left <= 0:
            return None
        try:
            m = q.get(timeout=left)
        except queue.Empty:
            return None
        if pred(m):
            return m
