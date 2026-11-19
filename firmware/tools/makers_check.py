#!/usr/bin/env python3
"""Refuses a makers' build (board/lite-makers) that carries anything private.

    makers_check.py BUILD            the makers' image: every item below absent
    makers_check.py --twin BUILD     its lab twin: the Hive key present, the rest absent
    makers_check.py --file F BUILD   scan F (a merged image, say) as well; repeatable

BUILD is the ESP-IDF build folder (DEST/build, with DEST/sdkconfig beside it).
Every .bin under it is read as bytes and searched for:

- the Hive's public key (remote_bench.py pubkey), as hex in either case and as raw bytes
- the bench's Ed25519 seed (~/.config/h2/bench-dev-ed25519), hex and raw
- the bench endpoint's host and Cloudflare Access team (~/.h2/bench-dev/config.json)
- the Access service token's id and secret (~/.config/h2/bench-dev-token.json)
- the Spotify developer key, the saved Wi-Fi network and its password
  (SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET, WIFI_NETWORK, WIFI_PASSWORD in
  HIFI_SECRETS_DIR, default ~/.config/h2/secrets)
- any --ssid given (the lab boards' networks)
- the LAN signature trailer at an image's end, which would mean the bench key signed it

and the config and link maps for the lab and Spotify parts: CONFIG_HIFI_LAB
and CONFIG_CSPOT_SINK off, no lab_*.c or hifi_lab_esp.c object and no cspot
code linked (a member of libcspot.a: the library itself stays on the link
line). An item whose source isn't on this machine is said to be unchecked,
not passed. Values are never printed, only each item's name.
Exit 0 when the scan passes, 1 when it finds anything (or, with --twin, when
the Hive key is missing: the twin is the positive control that the scan sees it).
"""
import argparse
import json
import os
import re
import subprocess
import sys
import urllib.parse
from pathlib import Path

HOME = Path.home()
SECRETS = Path(os.environ.get("HIFI_SECRETS_DIR") or HOME / ".config/h2/secrets")
if not SECRETS.is_dir():  # build.sh points the makers' build at no secrets; the scan still reads them
    SECRETS = HOME / ".config/h2/secrets"


def text(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def needles(value, raw_hex=False):
    """the byte strings a value may appear as in an image"""
    out = [value.encode(), value.lower().encode(), value.upper().encode()]
    if raw_hex and re.fullmatch(r"(?:[0-9a-fA-F]{2})+", value):
        out.append(bytes.fromhex(value))
    return sorted(set(out))


def items(ssids):
    """[(name, [needles] or None when its source isn't here)]"""
    out = []
    pk = None
    if (HOME / "modules/dash/remote_bench.py").exists():
        r = subprocess.run([sys.executable, str(HOME / "modules/dash/remote_bench.py"), "pubkey"],
                           capture_output=True, text=True)
        pk = r.stdout.strip() or None
    keys = [k.strip() for k in pk.split(",")] if pk else []
    out.append(("Hive public key", [n for k in keys for n in needles(k, True)] or None))
    seed = text(HOME / ".config/h2/bench-dev-ed25519")
    out.append(("bench signing seed", needles(seed, True) if seed else None))
    cfg = {}
    try:
        cfg = json.loads(Path(HOME / ".h2/bench-dev/config.json").read_text())
    except (OSError, ValueError):
        pass
    host = urllib.parse.urlparse(cfg.get("url", "")).hostname
    out.append(("bench endpoint host", needles(host) if host else None))
    out.append(("Cloudflare Access team", needles(cfg["team"]) if cfg.get("team") else None))
    tok = {}
    try:
        tok = json.loads(Path(HOME / ".config/h2/bench-dev-token.json").read_text())
    except (OSError, ValueError):
        pass
    for k in ("client_id", "client_secret"):
        out.append((f"Access token {k}", needles(tok[k]) if tok.get(k) else None))
    for k in ("SPOTIFY_CLIENT_ID", "SPOTIFY_CLIENT_SECRET", "WIFI_NETWORK", "WIFI_PASSWORD"):
        v = text(SECRETS / k)
        out.append((k, needles(v) if v else None))
    for i, s in enumerate(ssids):
        out.append((f"lab SSID #{i + 1}", needles(s)))
    return out


def config_checks(build):
    """[(name, ok)] for the sdkconfig and the link maps"""
    sdk = (build.parent / "sdkconfig").read_text()
    out = [("CONFIG_HIFI_LAB off", not re.search(r"^CONFIG_HIFI_LAB=y", sdk, re.M)),
           ("CONFIG_CSPOT_SINK off", not re.search(r"^CONFIG_CSPOT_SINK=y", sdk, re.M))]
    for m in sorted(build.glob("*.map")):
        t = m.read_text(errors="replace")
        out.append((f"no lab object in {m.name}", not re.search(r"\((?:lab_\w+|hifi_lab_esp)\.c\.obj\)", t)))
        out.append((f"no cspot code in {m.name}", not re.search(r"libcspot[\w-]*\.a\(", t)))  # a member pulled in, not just the library on the link line
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build", type=Path)
    ap.add_argument("--twin", action="store_true")
    ap.add_argument("--file", action="append", default=[], type=Path)
    ap.add_argument("--ssid", action="append", default=[])
    a = ap.parse_args()
    bins = sorted(p for p in a.build.rglob("*.bin") if "CMakeFiles" not in p.parts and "esp-idf" not in p.parts) + a.file
    if not bins:
        sys.exit(f"makers_check: no .bin under {a.build}")
    blobs = {p: p.read_bytes() for p in bins}
    print(f"makers_check: {len(bins)} images ({', '.join(str(p.relative_to(a.build)) if p.is_relative_to(a.build) else p.name for p in bins)})")
    bad = False
    for name, ns in items(a.ssid):
        if ns is None:
            bad |= a.twin and name == "Hive public key"
            print(f"  {name}: not checked (no source on this machine)")
            continue
        hits = [p.name for p, d in blobs.items() if any(n in d for n in ns)]
        want = a.twin and name == "Hive public key"
        if want:
            ok = "squeezelite.bin" in hits
            print(f"  {name}: {'present in ' + ', '.join(hits) + ' (the twin needs it)' if ok else 'MISSING from squeezelite.bin'}")
        else:
            ok = not hits
            print(f"  {name}: {'absent' if ok else 'FOUND in ' + ', '.join(hits)}")
        bad |= not ok
    for name, ok in config_checks(a.build):
        print(f"  {name}: {'yes' if ok else 'NO'}")
        bad |= not ok
    # the trailer sign_fw.py appends (hifi_ota.h): the app itself names the magic, so look only at the end
    signed = [p.name for p, d in blobs.items() if d[-80:-72] in (b"HIFISIG2",) or d[-72:-64] == b"HIFISIG1"]
    print(f"  LAN signature trailer: {'none' if not signed else 'FOUND on ' + ', '.join(signed)}")
    bad |= bool(signed)
    print(f"makers_check: {'FAILED' if bad else 'passed'}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
