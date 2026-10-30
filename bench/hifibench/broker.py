"""A tiny MQTT 3.1.1 broker for the bench's tests and `bench.py mock --relay`.

Plain TCP, one thread per client, clean sessions only. It keeps retained
messages, sends a client's last will when it drops without DISCONNECT (or
goes quiet past 1.5 x its keepalive), acks QoS 1 both ways and matches `+`
and `#`. An optional login table refuses everyone else. Not for real use:
the boxes in a home talk to a hosted broker (docs/remote.md).
"""
from __future__ import annotations

import socket
import struct
import threading

from .mqtt import (CONNACK, CONNECT, DISCONNECT, PINGREQ, PINGRESP, PUBACK, PUBLISH, SUBACK, SUBSCRIBE,
                   UNSUBACK, UNSUBSCRIBE, packet, parse_publish, publish_packet, read_packet, take_s16,
                   topic_matches)


class _Conn:
    def __init__(self, sock):
        self.sock = sock
        self.subs: dict[str, int] = {}
        self.will = None
        self.id = ""
        self.lock = threading.Lock()
        self.pid = 0

    def send(self, data: bytes):
        with self.lock:
            self.sock.sendall(data)

    def deliver(self, topic: str, payload: bytes, qos: int, retain: bool = False):
        with self.lock:
            pid = 0
            if qos:
                self.pid = self.pid % 65535 + 1
                pid = self.pid
            self.sock.sendall(publish_packet(topic, payload, qos, retain, pid))


class Broker:
    def __init__(self, host: str = "127.0.0.1", port: int = 0, logins: dict[str, str] | None = None):
        self.logins = logins
        self.retained: dict[str, tuple[bytes, int]] = {}
        self.conns: list[_Conn] = []
        self.lock = threading.Lock()
        self.srv = socket.create_server((host, port))
        self.host, self.port = self.srv.getsockname()[:2]
        self._stop = False
        threading.Thread(target=self._accept, daemon=True).start()

    @property
    def url(self) -> str:
        return f"mqtt://{self.host}:{self.port}"

    def close(self):
        self._stop = True
        self.srv.close()
        with self.lock:
            for c in list(self.conns):
                try:
                    c.sock.close()
                except OSError:
                    pass

    def _accept(self):
        while not self._stop:
            try:
                s, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(s,), daemon=True).start()

    def route(self, topic: str, payload: bytes, qos: int, retain: bool):
        if retain:
            with self.lock:
                if payload:
                    self.retained[topic] = (payload, qos)
                else:
                    self.retained.pop(topic, None)
        with self.lock:
            targets = [(c, max(q for f, q in c.subs.items() if topic_matches(f, topic)))
                       for c in self.conns if any(topic_matches(f, topic) for f in c.subs)]
        for c, q in targets:
            try:
                c.deliver(topic, payload, min(q, qos))
            except OSError:
                pass

    def _serve(self, s: socket.socket):
        c = _Conn(s)
        clean = False
        try:
            kind, _, b = read_packet(s)
            if kind != CONNECT:
                return
            _, i = take_s16(b, 0)
            flags = b[i + 1]
            keepalive = struct.unpack_from("!H", b, i + 2)[0]
            i += 4
            cid, i = take_s16(b, i)
            c.id = cid.decode()
            if flags & 0x04:
                wt, i = take_s16(b, i)
                wp, i = take_s16(b, i)
                c.will = (wt.decode(), wp, (flags >> 3) & 3, bool(flags & 0x20))
            user = pw = None
            if flags & 0x80:
                u, i = take_s16(b, i)
                user = u.decode()
            if flags & 0x40:
                p, i = take_s16(b, i)
                pw = p.decode()
            if self.logins is not None and self.logins.get(user or "") != pw:
                s.sendall(packet(CONNACK, 0, b"\x00\x04"))
                return
            s.sendall(packet(CONNACK, 0, b"\x00\x00"))
            s.settimeout(keepalive * 1.5 if keepalive else None)
            with self.lock:
                self.conns.append(c)
            while True:
                kind, flags, b = read_packet(s)
                if kind == PUBLISH:
                    t, p, q, r, pid = parse_publish(flags, b)
                    if q:
                        c.send(packet(PUBACK, 0, struct.pack("!H", pid)))
                    self.route(t, p, q, r)
                elif kind == SUBSCRIBE:
                    pid = struct.unpack_from("!H", b)[0]
                    i, granted, new = 2, b"", []
                    while i < len(b):
                        f, i = take_s16(b, i)
                        q = min(b[i], 1)
                        i += 1
                        c.subs[f.decode()] = q
                        granted += bytes([q])
                        new.append((f.decode(), q))
                    c.send(packet(SUBACK, 0, struct.pack("!H", pid) + granted))
                    with self.lock:
                        ret = list(self.retained.items())
                    for f, q in new:
                        for t, (p, rq) in ret:
                            if topic_matches(f, t):
                                c.deliver(t, p, min(q, rq), retain=True)
                elif kind == UNSUBSCRIBE:
                    pid = struct.unpack_from("!H", b)[0]
                    i = 2
                    while i < len(b):
                        f, i = take_s16(b, i)
                        c.subs.pop(f.decode(), None)
                    c.send(packet(UNSUBACK, 0, struct.pack("!H", pid)))
                elif kind == PINGREQ:
                    c.send(packet(PINGRESP, 0, b""))
                elif kind == DISCONNECT:
                    clean = True
                    return
        except (OSError, ConnectionError, ValueError, IndexError, struct.error):
            pass
        finally:
            with self.lock:
                if c in self.conns:
                    self.conns.remove(c)
            try:
                s.close()
            except OSError:
                pass
            if c.will and not clean and not self._stop:
                self.route(*c.will)
