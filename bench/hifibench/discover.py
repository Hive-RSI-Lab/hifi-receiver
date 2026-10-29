"""Find boards: CH340 serial ports on USB, and `_hifi._tcp` over mDNS.

Stdlib only. The mDNS browse sends one PTR query (asking for unicast
answers) and parses PTR, SRV, TXT and A records from whatever answers within
the timeout; it is enough for a bench LAN, not a general resolver.
"""
from __future__ import annotations

import glob
import os
import socket
import struct
import time
from dataclasses import dataclass, field

from . import CH340, SERVICE

MDNS = ("224.0.0.251", 5353)


# ---- USB ----------------------------------------------------------------

def serial_ports() -> list[str]:
    """Serial ports whose USB device is a CH340 (Linux sysfs, macOS names)."""
    out = []
    for tty in sorted(glob.glob("/sys/class/tty/ttyUSB*")):
        dev = os.path.realpath(os.path.join(tty, "device"))
        for up in (dev, os.path.dirname(dev), os.path.dirname(os.path.dirname(dev))):
            try:
                vid = int(open(os.path.join(up, "idVendor")).read(), 16)
                pid = int(open(os.path.join(up, "idProduct")).read(), 16)
            except OSError:
                continue
            if (vid, pid) == CH340:
                out.append("/dev/" + os.path.basename(tty))
            break
    out += sorted(glob.glob("/dev/cu.wchusbserial*") + glob.glob("/dev/cu.usbserial-*"))
    return out


# ---- mDNS ---------------------------------------------------------------

@dataclass
class Found:
    name: str
    host: str = ""
    port: int = 80
    addr: str = ""
    txt: dict = field(default_factory=dict)

    @property
    def url(self) -> str:
        return f"http://{self.addr or self.host.rstrip('.')}:{self.port}"


def _qname(name: str) -> bytes:
    return b"".join(bytes([len(p)]) + p.encode() for p in name.rstrip(".").split(".")) + b"\0"


def query(service: str = SERVICE) -> bytes:
    hdr = struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0)
    return hdr + _qname(service) + struct.pack("!HH", 12, 0x8001)  # PTR, IN + unicast-response


def _name(pkt: bytes, off: int) -> tuple[str, int]:
    parts, jumped, end = [], False, off
    for _ in range(64):
        n = pkt[off]
        if n == 0:
            off += 1
            break
        if n & 0xC0 == 0xC0:
            ptr = struct.unpack("!H", pkt[off:off + 2])[0] & 0x3FFF
            if not jumped:
                end = off + 2
            jumped, off = True, ptr
            continue
        parts.append(pkt[off + 1:off + 1 + n].decode(errors="replace"))
        off += 1 + n
    return ".".join(parts) + ".", (end if jumped else off)


def parse(pkt: bytes, found: dict[str, Found], service: str = SERVICE) -> None:
    """Fold one mDNS answer packet's records into found (by instance name)."""
    _, _, qd, an, ns, ar = struct.unpack("!HHHHHH", pkt[:12])
    off = 12
    for _ in range(qd):
        _, off = _name(pkt, off)
        off += 4
    hosts: dict[str, str] = {}
    srv: list[tuple[str, str, int]] = []
    for _ in range(an + ns + ar):
        name, off = _name(pkt, off)
        rtype, _, _, rdlen = struct.unpack("!HHIH", pkt[off:off + 10])
        off += 10
        rd = off
        off += rdlen
        if rtype == 12 and name.lower() == service.lower():
            inst, _ = _name(pkt, rd)
            found.setdefault(inst, Found(inst))
        elif rtype == 33:
            port = struct.unpack("!H", pkt[rd + 4:rd + 6])[0]
            target, _ = _name(pkt, rd + 6)
            srv.append((name, target, port))
        elif rtype == 16:
            f = found.setdefault(name, Found(name))
            i = rd
            while i < rd + rdlen:
                n = pkt[i]
                kv = pkt[i + 1:i + 1 + n].decode(errors="replace")
                k, _, v = kv.partition("=")
                if k:
                    f.txt[k] = v
                i += 1 + n
        elif rtype == 1 and rdlen == 4:
            hosts[name.lower()] = socket.inet_ntoa(pkt[rd:rd + 4])
    for name, target, port in srv:
        f = found.setdefault(name, Found(name))
        f.host, f.port = target, port
    for f in found.values():
        if not f.addr and f.host.lower() in hosts:
            f.addr = hosts[f.host.lower()]


def browse(timeout: float = 2.0, service: str = SERVICE) -> list[Found]:
    found: dict[str, Found] = {}
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        s.settimeout(0.2)
        s.sendto(query(service), MDNS)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                pkt, _ = s.recvfrom(9000)
            except socket.timeout:
                continue
            try:
                parse(pkt, found, service)
            except (struct.error, IndexError):
                continue
    except OSError:
        pass  # no multicast route (a sandbox, no network): nothing found
    finally:
        s.close()
    # A host that answered SRV but not A: let the OS resolve .local.
    for f in found.values():
        if f.host and not f.addr:
            try:
                f.addr = socket.gethostbyname(f.host.rstrip("."))
            except OSError:
                pass
    return [f for f in found.values() if f.host or f.addr]
