#!/usr/bin/env python3
"""Write an otadata image (the 0x2000 partition at 0xD000) that boots ota_0.

ESP-IDF's build writes ota_data_initial.bin as all 0xff, so a board flashed
from scratch boots the factory app: squeezelite's recovery, which has no
Ethernet, no audio and is only meant to write an image into an OTA slot. The
main app is flashed into ota_0 alongside it, so point the bootloader there.
Recovery stays the fallback: `recovery` from the main app's console, a
rollback, or squeezelite's crash counter all still reach it.

The state is VALID, not NEW: a board flashed over USB is not an OTA on trial,
and NEW would put it through the trial (components/hifi/core/hifi_boot.h),
which fails on a bench without PD power and would roll it back to recovery.

Layout (esp_ota_select_entry_t, ESP-IDF v4.4 esp_flash_partitions.h): two
4 KiB sectors, each starting with
    uint32 ota_seq; uint8 seq_label[20]; uint32 ota_state; uint32 crc
where crc = crc32_le(0xffffffff, ota_seq) as otatool.py computes it. The
bootloader boots slot (max valid seq - 1) % number of OTA slots.

    otadata.py OUT [--slot N]
"""
import argparse
import binascii
import struct
import sys

SECTOR = 0x1000
ESP_OTA_IMG_VALID = 2


def entry(seq, state=ESP_OTA_IMG_VALID):
    crc = binascii.crc32(struct.pack("<I", seq), 0xFFFFFFFF) & 0xFFFFFFFF
    return struct.pack("<I20sII", seq, b"\xff" * 20, state, crc)


def image(slot=0):
    """otadata whose sector 0 selects ota_<slot>; sector 1 stays erased."""
    e = entry(slot + 1)
    return e + b"\xff" * (SECTOR - len(e)) + b"\xff" * SECTOR


def parse(data):
    """(seq, state) of each sector whose crc checks out, as the bootloader reads them."""
    out = []
    for i in range(2):
        seq, _label, state, crc = struct.unpack_from("<I20sII", data, i * SECTOR)
        ok = seq != 0xFFFFFFFF and crc == binascii.crc32(struct.pack("<I", seq), 0xFFFFFFFF) & 0xFFFFFFFF
        out.append((seq, state) if ok else None)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("--slot", type=int, default=0)
    a = ap.parse_args()
    with open(a.out, "wb") as f:
        f.write(image(a.slot))
    print(f"{a.out}: otadata selecting ota_{a.slot}")


if __name__ == "__main__":
    sys.exit(main())
