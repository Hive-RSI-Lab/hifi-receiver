#!/usr/bin/env python3
"""Writes the checked-in seed inputs under fuzz/seeds/<harness>/: one valid
request for each route and command, so the fuzzers start past the parsers'
first checks. Rerun it after adding a route: python3 fuzz/seeds.py
(regress-* files there are crashes the fuzzers found, kept by hand.)"""
import base64
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent / "seeds"
P = "/api/v1/"

# (method index: GET 0, POST 1, PUT 2, DELETE 3, plus 8: an OTA of 1024 bytes open; route; body or None)
API = [
    (0, "info", None), (0, "state", None), (0, "ota", None), (0, "crash", None), (0, "wifi/scan", None),
    (0, "update/check", None), (0, "remote", None),
    (1, "tone", '{"type":"sine","freq_hz":1000,"level_dbfs":-20,"channel":"both","duration_s":5}'),
    (1, "tone", '{"type":"sweep","start_hz":20,"end_hz":20000,"level_dbfs":-20,"duration_s":10}'),
    (1, "tone", '{"type":"noise","level_dbfs":-30,"duration_s":8}'), (1, "tone", '{"type":"stop"}'),
    (1, "amp", '{"enable":true}'), (1, "input", '{"input":"dj_direct"}'), (1, "sub", '{"level_db":6,"on":true}'),
    (1, "eq", '{"gains":[0,1,-2,3,0,0,0,0,0,-12]}'), (1, "name", '{"name":"Lite"}'),
    (1, "reboot", '{}'), (1, "reset", '{"factory":true}'), (1, "control", '{"action":"volup"}'),
    (1, "volume", '{"volume":40}'), (1, "ir/learn", '{"action":"mute"}'), (1, "ir/forget", '{"action":"volup"}'),
    (1, "selftest", None), (1, "crash", '{"from":0}'), (1, "bttest", '{"sink":"Lite 2"}'),
    (1, "ota/begin", '{"size":1024,"sha256":"' + "ab" * 32 + '","version":"1.0"}'),
    (1 | 8, "ota/chunk", '{"off":0,"data":"' + base64.b64encode(b"\xe9" + bytes(1023)).decode() + '"}'),
    (1 | 8, "ota/finish", '{"reboot":false}'), (1 | 8, "ota/abort", '{}'),
    (1, "capture", '{"source":"line","frames":256}'), (1, "wifi", '{"ssid":"x","password":"12345678"}'),
    (1, "wifi/forget", None), (1, "zone", '{"zone":"b","on":true}'), (1, "led", '{"mode":"test"}'),
    (1, "backlight", '{"mode":"auto"}'), (1, "update", '{"url":"http://hive.local/fw/lite.bin"}'),
    (1, "remote", '{"url":"wss://relay.example.net/r","user":"hifi","password":"pw","key":"cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"}'), (2, "volume", '{"volume":1}'), (3, "info", None),
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
    # byte 0: method in bits 0-1, every optional op in bit 2, an open OTA in bit 3
    write("fuzz_api", [bytes([m | 4]) + (P + r + ("\n" + b if b is not None else "")).encode()
                       for m, r, b in API] + [bytes([0]) + (P + "info").encode()])
    # byte 0: bit 0 every optional op, bit 1 audio playing
    write("fuzz_cli", [bytes([1 | (i & 2)]) + c.encode() for i, c in enumerate(CLI)])
    # byte 0: the key in bits 0-2, the writer's buffer (16 bytes a step) in bits 3-7
    write("fuzz_json", [bytes([i | 0xf8]) + j.encode() for i, j in enumerate(JSON)])
    write("fuzz_ws", WS)
    sig = base64.b64encode(b"\x5a" + bytes(63)).decode()
    rem = []
    for i, (m, r, b) in enumerate(API[:12]):
        cmd = {"v": 1, "id": 1761700000001 + i, "ts": 1761700000, "dev": "240ac4123456",
               "method": ["GET", "POST", "PUT", "DELETE"][m & 3], "path": P + r}
        if b is not None:
            cmd["body"] = json.loads(b)
        # byte 0: bit 0 no key, bit 1 an older id already taken, bit 2 the clock set
        rem.append(bytes([4 if i % 2 else 0]) + (json.dumps(cmd, separators=(",", ":")) + "\n" + sig).encode())
    write("fuzz_remote", rem)
    # the radios (fuzz_link.h): byte 0 the role (0 echo, 1 ping, 2 flood) and bit 2 a clock
    # from boot; then per frame a gap, a byte of flags (fuzz_espnow: bit 0 a ping of ours
    # went out; fuzz_ble: the event, 1 a value write or notification) and a frame built from
    # fields: 3 (plausible times; | 32 under the forger's key, | 4 a 4-byte seq), type (0 PING,
    # 1 PONG, 2 DATA), session (0 ours, 1 the receiver's live one), seq, the time back (4 bytes),
    # the echo's turnaround (/50 us), its clock offset (4), length - HIFI_LINK_MIN (46)
    def frame(typ: int, seq: int, size: int = 100, forged: bool = False, session: int = 0) -> bytes:
        k = 3 | (32 | 4 if forged else 0)
        s = seq.to_bytes(4, "little") if forged else bytes([seq])
        return bytes([k, typ, session]) + s + (2000).to_bytes(4, "little") + bytes([4]) + \
            (1500).to_bytes(4, "little") + bytes([size])
    # full frames: 250 - 46 on ESP-NOW, HIFI_LINK_BLE_PAYLOAD (244) - HIFI_LINK_MIN (46) on BLE
    raw = bytes([0, 40]) + b"HL\x02\x01" + bytes(36)  # an untagged frame of the last version
    for name, flag in (("fuzz_espnow", 1), ("fuzz_ble", 1)):
        write(name, [bytes([0]) + b"".join(bytes([3, flag]) + frame(0, q) for q in range(8)),
                     bytes([0]) + b"".join(bytes([1, flag]) + frame(2, q, 204 if name == "fuzz_espnow" else 198) for q in range(16)),
                     bytes([1]) + b"".join(bytes([20, flag]) + frame(1, q) for q in range(24)),
                     bytes([2 | 4]) + b"".join(bytes([5, flag]) + frame(1, q) for q in range(4)),
                     bytes([0]) + bytes([3, flag]) + raw,
                     # a stranger's frames at an echo mid-run: the live session, a jump in seq,
                     # then the next seq, a PING and the same seq; all bad, nothing else moves
                     bytes([0]) + b"".join(bytes([1, flag]) + frame(2, q) for q in range(8)) +
                     b"".join(bytes([1, flag]) + frame(t, q, forged=True, session=1)
                              for t, q in ((2, 1000000), (2, 8), (0, 9), (2, 7)))])
    # the forger's parser (fuzz_espnow only): the whole input is one ESP-NOW action frame
    # off the air, from the MAC at bytes 10-15, its vendor element around a 100-byte body
    oui = b"\x18\xfe\x34"
    air = b"\xd0" + bytes(3) + b"\xff" * 6 + bytes.fromhex("3076f52d742c") + b"\xff" * 6 + bytes(2) + \
        b"\x7f" + oui + bytes(4) + bytes([221, 105]) + oui + b"\x04\x01" + bytes(range(100))
    assert len(air) == 39 + 100
    (HERE / "fuzz_espnow" / "seed-air").write_bytes(air)


if __name__ == "__main__":
    main()
