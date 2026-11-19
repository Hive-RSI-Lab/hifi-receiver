#!/usr/bin/env python3
"""The makers' install file: one merged image and its ESP Web Tools manifest.

    makers_image.py [--dirty] BUILD OUTDIR

BUILD is the makers' ESP-IDF build folder (board/lite-makers, built by
tools/build.sh). Writes to OUTDIR:

- hifi-lite-makers.bin: esptool merge_bin of every image in BUILD/flash_args
  (bootloader 0x1000, partition table 0x8000, otadata 0xd000, recovery
  0x10000, main app 0x1d0000), flashed at 0 from a browser
- manifest.json: ESP Web Tools' manifest for it (chip family ESP32, one part
  at offset 0, new_install_prompt_erase so a first install starts clean)
- hifi-lite-makers.json: what went in, each part's offset, size and SHA-256,
  the merged file's SHA-256, and the source it was built from (this repo's
  commit, and squeezelite-esp32's commit and the IDF from upstream.lock), for
  the page, the GPL's source offer and checking a download. A tree with
  uncommitted changes under firmware/ is refused unless --dirty is given,
  and is then marked dirty.

Before merging it checks every offset in flash_args against the partition
table the build used (partitions-hifi.csv beside BUILD) and that each image
fits its slot. The merged file is kept out of git (docs/install/README.md).
"""
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

NAME = "hifi-lite-makers.bin"
FIXED = {"bootloader/bootloader.bin": (0x1000, 0x7000), "partition_table/partition-table.bin": (0x8000, 0x1000)}


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def partitions(csv):
    out = {}
    for line in Path(csv).read_text().splitlines():
        f = [x.strip() for x in line.split("#")[0].split(",")]
        if len(f) >= 5 and f[0]:
            out[f[0]] = (f[1], f[2], int(f[3], 0), int(f[4], 0))
    return out


def esptool():
    """esptool as the IDF ships it (v3.x has merge_bin), or the one on PATH"""
    idf = os.environ.get("IDF_PATH")
    if idf and Path(idf, "components/esptool_py/esptool/esptool.py").exists():
        return [sys.executable, str(Path(idf, "components/esptool_py/esptool/esptool.py"))]
    return [sys.executable, "-m", "esptool"]


def source():
    """the commits this build came from: {repo, commit, dirty, upstream: {...}}"""
    fw = Path(__file__).resolve().parent.parent
    git = lambda *a: subprocess.run(["git", "-C", str(fw), *a], capture_output=True, text=True).stdout.strip()
    lock = dict(l.split("=", 1) for l in (fw / "upstream.lock").read_text().splitlines()
                if "=" in l and not l.startswith("#"))
    return {"repo": "https://github.com/Hive-RSI-Lab/hifi-receiver", "commit": git("rev-parse", "HEAD"),
            "dirty": bool(git("status", "--porcelain", "--", ".")),
            "upstream": {k: lock.get(k) for k in ("url", "commit", "idf")}}


def main():
    args = [a for a in sys.argv[1:] if a != "--dirty"]
    if len(args) != 2:
        sys.exit(__doc__)
    build, out = Path(args[0]), Path(args[1])
    src = source()
    if src["dirty"] and "--dirty" not in sys.argv:
        sys.exit("makers_image: firmware/ has uncommitted changes, so the image wouldn't match a commit; "
                 "commit first, or pass --dirty to mark it")
    words = shlex.split((build / "flash_args").read_text())
    flags = [w for w in words[:6]]
    pairs = [(int(words[i], 0), words[i + 1]) for i in range(6, len(words), 2)]
    if flags[::2] != ["--flash_mode", "--flash_freq", "--flash_size"]:
        sys.exit(f"makers_image: unexpected flash_args header {flags}")
    parts = partitions(build.parent / "partitions-hifi.csv")
    slot = {"recovery.bin": "recovery", "squeezelite.bin": "ota_0", "ota_data_initial.bin": "otadata"}
    bad = []
    info = []
    for off, img in sorted(pairs):
        size = (build / img).stat().st_size
        if img in FIXED:
            want, cap = FIXED[img]
        elif img in slot and slot[img] in parts:
            want, cap = parts[slot[img]][2], parts[slot[img]][3]
        else:
            bad.append(f"{img}: no partition to check it against")
            continue
        ok = off == want and size <= cap
        print(f"  {off:#08x} {img}: {size} bytes, slot {want:#x}+{cap:#x}: {'ok' if ok else 'WRONG'}")
        if not ok:
            bad.append(img)
        info.append({"offset": f"{off:#x}", "file": img, "size": size, "sha256": sha(build / img)})
    if bad:
        sys.exit(f"makers_image: offsets or sizes don't match the partition table: {bad}")
    out.mkdir(parents=True, exist_ok=True)
    merged = out / NAME
    subprocess.run(esptool() + ["--chip", "esp32", "merge_bin", "-o", str(merged)] + flags +
                   [x for off, img in pairs for x in (f"{off:#x}", str(build / img))], check=True)
    # the version the app reports (squeezelite's PROJECT_VER and our build number)
    app = (build / "squeezelite.bin").read_bytes()
    m = re.search(rb"HIFI-BUILD:(\d+);", app)
    build_no = m.group(1).decode() if m else "unknown"
    manifest = {
        "name": "HiFi Lite (makers' build)",
        "version": build_no,
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,  # no Improv: the board onboards by its setup AP
        "builds": [{"chipFamily": "ESP32", "parts": [{"path": NAME, "offset": 0}]}],
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (out / "hifi-lite-makers.json").write_text(json.dumps({
        "build": build_no, "source": src, "flash": dict(zip(flags[::2], flags[1::2])), "parts": info,
        "merged": {"file": NAME, "size": merged.stat().st_size, "sha256": sha(merged)}}, indent=2) + "\n")
    print(f"makers_image: {merged} ({merged.stat().st_size} bytes, sha256 {sha(merged)}), build {build_no}, "
          f"source {src['commit'][:12]}{' (dirty)' if src['dirty'] else ''}")


if __name__ == "__main__":
    main()
