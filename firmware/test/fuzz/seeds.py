#!/usr/bin/env python3
"""Writes the checked-in seed inputs under fuzz/seeds/<harness>/: one valid
request for each route and command, so the fuzzers start past the parsers'
first checks. Rerun it after adding a route: python3 fuzz/seeds.py"""
import base64
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent / "seeds"
P = "/api/v1/"

# (method index: GET 0, POST 1, PUT 2, DELETE 3; route; body or None)
API = [
    (0, "info", None), (0, "state", None), (0, "ota", None), (0, "crash", None), (0, "wifi/scan", None),
    (1, "tone", '{"type":"sine","freq_hz":1000,"level_dbfs":-20,"channel":"both","duration_s":5}'),
    (1, "tone", '{"type":"sweep","start_hz":20,"end_hz":20000,"level_dbfs":-20,"duration_s":10}'),
    (1, "tone", '{"type":"noise","level_dbfs":-30,"duration_s":8}'), (1, "tone", '{"stop":true}'),
    (1, "amp", '{"on":true}'), (1, "input", '{"input":"bt"}'), (1, "sub", '{"crossover_hz":80}'),
    (1, "eq", '{"bands":[0,1,-2,3,0,0,0,0,0,0]}'), (1, "name", '{"name":"Lite"}'),
    (1, "reboot", '{}'), (1, "reset", '{"confirm":true}'), (1, "control", '{"action":"next"}'),
    (1, "volume", '{"volume":40}'), (1, "ir/learn", '{"button":"vol_up"}'), (1, "ir/forget", '{"button":"all"}'),
    (1, "selftest", None), (1, "crash", '{"from":0}'), (1, "bttest", '{"on":true}'),
    (1, "ota/begin", '{"size":1048576,"sha256":"' + "ab" * 32 + '"}'), (1, "ota/end", '{}'),
    (1, "capture", '{"seconds":1}'), (1, "wifi", '{"ssid":"x","password":"12345678"}'),
    (1, "wifi/forget", None), (1, "zone", '{"zone":"kitchen"}'), (1, "led", '{"on":false}'),
    (1, "backlight", '{"level":50}'), (2, "volume", '{"volume":1}'), (3, "info", None),
]

CLI = ["info", "state", "crash", "crash 3", "selftest", "amp on", "input bt", "sub 80", "control next",
       "control prev", "volume 30", "maxvol 80", "mute on", "ir learn vol_up", "ir forget", "led on",
       "backlight 40", "tone sine 1000 -20 both 5", "tone sweep 20 20000", "tone stop", "i2c 0x28 0x00",
       "i2c 0x28 0x00 0x12", "help"]

JSON = ['{"a":1,"b":"x","c":[1,2,{"d":null}],"e":true}', '{"s":"\\u00e9\\n\\"q\\""}',
        '{"n":-1.5e3,"o":{"p":{"q":[]}}}', '{"version":"1.2.3","url":"https://x/y.bin","sha256":"' + "cd" * 32 + '"}']

WS = [b"\x81\x05hello", b"\x82\x7e\x01\x00", b"\x88\x02\x03\xe8", b"\x89\x00", b"\x82\x7f" + bytes(8),
      b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      b"Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n",
      b"wss://relay.example.net/hive/board", b"wss://10.0.0.2:8443/a/b?c=d", b"dGhlIHNhbXBsZSBub25jZQ=="]


def write(name: str, items: list[bytes]) -> None:
    d = HERE / name
    d.mkdir(parents=True, exist_ok=True)
    for old in d.glob("seed-*"):
        old.unlink()
    for i, b in enumerate(items):
        (d / f"seed-{i:02d}").write_bytes(b)


def main() -> None:
    # byte 0: method in bits 0-1, every optional op in bit 2
    write("fuzz_api", [bytes([m | 4]) + (P + r + ("\n" + b if b is not None else "")).encode()
                       for m, r, b in API] + [bytes([0]) + (P + "info").encode()])
    write("fuzz_cli", [c.encode() for c in CLI])
    write("fuzz_json", [bytes([i]) + j.encode() for i, j in enumerate(JSON)])
    write("fuzz_ws", WS)
    sig = base64.b64encode(b"\x5a" + bytes(63)).decode()
    rem = []
    for i, (m, r, b) in enumerate(API[:12]):
        cmd = {"v": 1, "id": 1761700000001 + i, "ts": 1761700000, "dev": "240ac4123456",
               "method": ["GET", "POST", "PUT", "DELETE"][m], "path": P + r}
        if b is not None:
            cmd["body"] = json.loads(b)
        # byte 0: bit 0 no key, bit 1 an older id already taken, bit 2 the clock set
        rem.append(bytes([4 if i % 2 else 0]) + (json.dumps(cmd, separators=(",", ":")) + "\n" + sig).encode())
    write("fuzz_remote", rem)


if __name__ == "__main__":
    main()
