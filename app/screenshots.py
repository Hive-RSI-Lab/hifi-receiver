#!/usr/bin/env python3
"""Screenshots of the app against the mock, at a phone's size, for docs/app.md.

    python3 app/screenshots.py [--out docs/app]

Needs Google Chrome or Chromium on PATH; the Speakers, party and pairing
shots (against the mock's --house, clicked through by app/tests/drive.mjs)
also need Node 22 or later, and are skipped without it.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

APP = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(APP / "mock"))
import server  # noqa: E402

SHOTS = [  # file, mock settings, page[, window height]
    ("play", {}, "play"),
    ("sound", {}, "sound"),
    ("setup", {}, "setup", 2300),
    ("first-run", {"ap": True}, ""),
    ("weak-charger", {"pdo": (5.0, 3.0)}, "play"),
    ("fault", {"fault": True}, "play"),
    ("plus-sound", {"model": "plus"}, "sound", 1500),
    ("ultra-play", {"model": "ultra"}, "play"),
]

# against the house (Kitchen leads, Living room follows, Bedroom has a phone
# on Bluetooth, Garage is off), from Kitchen: drive.mjs steps with {shot}
HOUSE_SHOTS = [
    ("", [{"see": "Bedroom"}, {"see": "Follows Kitchen"}, {"shot": "speakers.png"},
          {"click": "Party ·"}, {"see": "In step"}, {"shot": "party-on.png"},
          {"click": "Stop party"}, {"gone": "Leads party"}, {"click": "Start a party"},
          {"click": "Start a party"}, {"see": "Who leads?"}, {"shot": "party-lead.png"}]),
    ("setup", [{"click": "Pair a new phone"}, {"see": "left to pair"},
               {"js": "document.querySelector('.pairbox').scrollIntoView({block: 'center'})"},
               {"shot": "pairing.png"}]),
]


def house_shots(chrome: str, out: Path) -> None:
    node = shutil.which("node")
    ok = node and subprocess.run([node, "-e", "process.exit(typeof WebSocket === 'function' ? 0 : 1)"]).returncode == 0
    if not ok:
        print("no Node 22+: the Speakers shots are skipped", file=sys.stderr)
        return
    for page, steps in HOUSE_SHOTS:
        srvs = server.serve_house(0)
        for s in srvs:
            threading.Thread(target=s.serve_forever, daemon=True).start()
        url = f"http://127.0.0.1:{srvs[0].server_address[1]}/" + (f"#{page}" if page else "")
        steps = [{**s, "shot": str(out / s["shot"])} if "shot" in s else s for s in steps]
        r = subprocess.run([node, str(APP / "tests" / "drive.mjs"), chrome, url, json.dumps(steps)],
                           capture_output=True, text=True, timeout=240)
        for s in srvs:
            s.shutdown()
            s.server_close()
        if r.returncode:
            raise SystemExit(f"drive.mjs failed:\n{r.stdout}{r.stderr}")
        for s in steps:
            if "shot" in s:
                print(s["shot"])


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=APP.parent / "docs" / "app")
    o = ap.parse_args(argv)
    chrome = next((shutil.which(c) for c in ("google-chrome", "chromium", "chromium-browser") if shutil.which(c)), None)
    if not chrome:
        print("no Chrome on PATH", file=sys.stderr)
        return 1
    o.out.mkdir(parents=True, exist_ok=True)
    for name, kw, page, *tall in SHOTS:
        srv = server.serve(0, **{"model": "lite", "ap": False, "pdo": (20.0, 3.25), "fault": False, **kw})
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        url = f"http://127.0.0.1:{srv.server_address[1]}/" + (f"#{page}" if page else "")
        with tempfile.TemporaryDirectory() as prof:
            # 390 CSS px wide is an iPhone 12-15; the window is tall enough for the whole page.
            subprocess.run([chrome, "--headless=new", "--no-sandbox", "--disable-gpu", "--no-proxy-server",
                            "--hide-scrollbars", f"--user-data-dir={prof}", f"--window-size=390,{tall[0] if tall else 1100}",
                            "--virtual-time-budget=3000", f"--screenshot={o.out / (name + '.png')}", url],
                           capture_output=True, timeout=240, check=True)
        srv.shutdown()
        srv.server_close()
        print(o.out / (name + ".png"))
    house_shots(chrome, o.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
