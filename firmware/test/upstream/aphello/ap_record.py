#!/usr/bin/env python3
"""Record one APResponseMessage packet (length prefix included) from a Spotify AP.

    ap_record.py OUT.bin

Makes test_up_aphello.py's fixture: a ClientHello with a throwaway key, and the
access point's answer (its public DH half, signed). No account is involved."""
import json, os, socket, struct, sys, urllib.request

def varint(n):
    out = b""
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out += bytes([b | 0x80])
        else:
            return out + bytes([b])

def f_var(no, v): return varint(no << 3) + varint(v)
def f_bytes(no, b): return varint(no << 3 | 2) + varint(len(b)) + b

build = f_var(10, 0) + f_var(30, 2) + f_var(40, 117300517)
dh = f_bytes(10, os.urandom(95) + b"\x02") + f_var(20, 1)
hello = (f_bytes(10, build) + f_var(30, 0) + f_bytes(50, f_bytes(10, dh)) +
         f_bytes(60, os.urandom(16)) + f_bytes(70, b"\x1e"))
aps = json.load(urllib.request.urlopen("https://apresolve.spotify.com/?type=accesspoint", timeout=20))["accesspoint"]
host, port = aps[0].rsplit(":", 1)
s = socket.create_connection((host, int(port)), timeout=20)
s.sendall(b"\x00\x04" + struct.pack(">I", 2 + 4 + len(hello)) + hello)
def rd(n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise SystemExit("closed")
        b += c
    return b
head = rd(4)
n = struct.unpack(">I", head)[0]
pkt = head + rd(n - 4)
open(sys.argv[1], "wb").write(pkt)
print(aps[0], len(pkt))
