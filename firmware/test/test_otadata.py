#!/usr/bin/env python3
"""tools/otadata.py writes an otadata image the ESP-IDF v4.4 bootloader reads
as 'boot ota_0, state VALID' (run by make -C firmware/test)."""
import binascii
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import otadata  # noqa: E402

d = otadata.image()
assert len(d) == 0x2000, len(d)
# the bytes the bench flashes: seq 1, blank label, VALID, crc as IDF's otatool computes it
want = struct.pack("<I", 1) + b"\xff" * 20 + struct.pack("<I", 2)
want += struct.pack("<I", binascii.crc32(struct.pack("<I", 1), 0xFFFFFFFF) % (1 << 32))
assert d[:32] == want, d[:32].hex()
assert d[:32].hex() == "01000000" + "ff" * 20 + "02000000" + "9a984347", d[:32].hex()
assert set(d[32:]) == {0xFF}
assert otadata.parse(d) == [(1, 2), None]
# bootloader: slot = (seq - 1) % 2 slots, so --slot 1 picks ota_1
assert otadata.parse(otadata.image(1))[0] == (2, 2)
assert otadata.parse(b"\xff" * 0x2000) == [None, None]
print("test_otadata: ok")
