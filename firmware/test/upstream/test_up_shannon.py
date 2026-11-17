#!/usr/bin/env python3
"""Regression: Spotify's access-point link refuses what isn't the access point's (patch 0026).

    test_up_shannon.py UPSTREAM_DIR [--plain FILE] [--shannon FILE]   (the patched tree upstream.py made)

cspot talks to Spotify's access point over a plain TCP socket: the handshake
answer comes in as a length-prefixed packet (PlainConnection::recvPacket),
then every packet is Shannon-encrypted with a MAC (ShannonConnection). Patch
0025 checks the access point's signed key, but three holes sat around it,
open to anyone on the path:

  - a handshake size below its own 4 bytes wrapped to about 4 GB, and the
    socket's bytes went past a 0-3 byte buffer (a heap overrun before the
    signature is ever looked at);
  - a packet whose MAC didn't match was logged and then parsed anyway;
  - a refused connect() threw before the address list was freed, one leak
    per try, and the player retries about once a second.

The test builds upstream's PlainConnection.cpp and ShannonConnection.cpp as
patched, with Shannon.cpp and bell's BellLogger.cpp, under ASan, UBSan and
LeakSanitizer, and runs shannon/harness.cpp's cases over loopback TCP, one
process each. --plain / --shannon build another PlainConnection.cpp /
ShannonConnection.cpp in their place: upstream's pristine ones overrun on
short-size, take other-key, and leak on refused-port, which is the bug.

huge-size (a 16 MB size) is refused before and after (upstream's read runs
dry and gives up); after the patch it is refused before any buffer is made.

Skipped, loudly, without a C++ compiler; UPSTREAM_REQUIRED=1 makes that a
failure.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
CSPOT = "components/spotify/cspot"
WANT = {"real": "took", "other-key": "refused", "short-size": "refused", "huge-size": "refused",
        "good-size": "took", "refused-port": "refused"}


def skip(why: str) -> int:
    print(f"SKIPPED test_up_shannon: {why}")
    return 1 if os.environ.get("UPSTREAM_REQUIRED", "0") != "0" else 0


def arg(args: list, flag: str, dflt: Path) -> Path:
    return Path(args[args.index(flag) + 1]).resolve() if flag in args else dflt


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__, file=sys.stderr)
        return 2
    up = Path(args[0]).resolve()
    cs = up / CSPOT
    plain = arg(args, "--plain", cs / "src/PlainConnection.cpp")
    shannon = arg(args, "--shannon", cs / "src/ShannonConnection.cpp")
    cxx = shutil.which("c++") or shutil.which("g++")
    if not cxx:
        return skip("needs a C++ compiler")
    for f in (plain, shannon, cs / "src/Shannon.cpp", cs / "bell/main/utilities/BellLogger.cpp"):
        if not f.is_file():
            print(f"test_up_shannon: no {f}")
            return 1
    with tempfile.TemporaryDirectory(prefix="up_shannon") as tmp:
        exe = Path(tmp) / "harness"
        cmd = [cxx, "-std=gnu++20", "-g", "-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
               "-I", cs / "include", "-I", cs / "bell/main/utilities/include",
               HERE / "shannon/harness.cpp", plain, shannon, cs / "src/Shannon.cpp",
               cs / "bell/main/utilities/BellLogger.cpp", "-o", exe, "-lpthread"]
        r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
        if r.returncode:
            print(f"test_up_shannon: build failed\n{r.stderr[-3000:]}")
            return 1
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:exitcode=66",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        fails = 0
        for case, want in WANT.items():
            try:
                r = subprocess.run([str(exe), case], capture_output=True, text=True, env=env, timeout=60)
                out, rc = r.stdout + r.stderr, r.returncode
            except subprocess.TimeoutExpired:
                out, rc = "timed out", -1
            line = next((l for l in r.stdout.splitlines() if l.startswith(case + ":")), "") if rc != -1 else ""
            got = line.split(":", 1)[1].strip().split(" ")[0] if line else "crashed"
            if rc != 0:
                san = next((l for l in out.splitlines() if "Sanitizer" in l and "ERROR" in l), out[-200:])
                got = f"crashed ({san.strip()[:160]})"
            ok = got == want
            fails += not ok
            print(f"  {'ok  ' if ok else 'FAIL'} {case}: {got}" + ("" if ok else f" (want {want})"))
        print(f"test_up_shannon: {len(WANT) - fails}/{len(WANT)} passed")
        return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
