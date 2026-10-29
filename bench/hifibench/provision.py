"""provision.py: the one command the person runs on their laptop
(docs/remote.md, "The flow"). With the box on the laptop's USB-C port it:

1. finds the board's CH340 serial port (or takes --port),
2. flashes it when given --image (JLC ships the ESP32 blank),
3. sets the Wi-Fi (asked for, or --wifi SSID:PASSWORD),
4. sets the relay login from the relay file's board_user/board_password,
5. waits until the board is on Wi-Fi and, with a relay, connected to it,
6. prints the board's id: after that it can be left plugged in.
"""
from __future__ import annotations

import argparse
import getpass
import sys
import time

from . import discover, flash, relay
from .api import ApiError, Console
from .serialport import open_port


def _say(msg: str):
    print(msg, flush=True)


def main(argv=None, ask=input, ask_secret=getpass.getpass, say=_say) -> int:
    ap = argparse.ArgumentParser(prog="provision.py", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="the board's serial port (default: the first CH340 found)")
    ap.add_argument("--image", help="flash this first: an ESP-IDF build dir or a merged .bin")
    ap.add_argument("--esptool", help="the esptool command (default: python -m esptool)")
    ap.add_argument("--wifi", help="SSID:PASSWORD (default: asked for)")
    ap.add_argument("--relay", help="relay.json (default: $HIFI_RELAY or ~/.config/hifi-receiver/relay.json)")
    ap.add_argument("--no-relay", action="store_true", help="Wi-Fi only (the board's image carries the relay login)")
    ap.add_argument("--timeout", type=float, default=90, help="seconds to wait for Wi-Fi and the relay")
    a = ap.parse_args(argv)

    port = a.port or (discover.serial_ports() or [None])[0]
    if not port:
        say("No board found on USB. Plug it into this laptop's USB-C port with a data cable "
            "(on Linux you may need to be in the 'dialout' group), or give --port.")
        return 2
    say(f"board on {port}")
    if a.image:
        say("flashing " + a.image)
        if flash.flash(port, a.image, esptool=a.esptool) != 0:
            say("flashing failed: is esptool installed (pip install esptool)?")
            return 2
        time.sleep(3)   # it boots after the flash

    dev = Console(open_port(port), timeout=5, name=port)
    try:
        info = dev.call("info")
    except ApiError:
        say(f"The board on {port} doesn't answer. If it is new it has no firmware yet: run this again with "
            "--image firmware/build (or the merged .bin the Hive gave you).")
        return 2
    dev.timeout = 15
    did = relay.dev_id(info.get("mac", ""))
    say(f"found {info.get('model', info.get('board', '?'))} {info.get('mac', '')}, firmware "
        f"{info.get('fw', info.get('fw_version', '?'))}")

    cfg = None
    if not a.no_relay:
        try:
            cfg = relay.load_config(a.relay)
        except FileNotFoundError as e:
            if a.relay:
                say(str(e))
                return 2
            say("no relay settings here: setting Wi-Fi only (the board's firmware must carry the relay login)")
    if a.wifi:
        ssid, _, pw = a.wifi.partition(":")
    else:
        ssid = ask("Wi-Fi network name: ").strip()
        pw = ask_secret("Wi-Fi password: ")
    if not ssid:
        say("no Wi-Fi name given")
        return 2
    dev.call("wifi", {"ssid": ssid, "password": pw}, post=True)
    say(f"Wi-Fi set to {ssid!r}")
    want_relay = False
    if cfg and cfg.get("board_user"):
        dev.call("remote", {"url": cfg["url"], "user": cfg["board_user"], "password": cfg.get("board_password"),
                            "prefix": cfg.get("prefix", "hifi")}, post=True)
        say(f"relay set to {cfg['url']}")
        want_relay = True
    elif cfg:
        say("relay.json has no board_user: the board keeps the relay login its firmware carries")

    end = time.monotonic() + a.timeout
    ip = connected = None
    while time.monotonic() < end:
        try:
            st = dev.call("state")
            ip = (st.get("net") or {}).get("ip")
            if ip:
                try:
                    r = dev.call("remote")
                    connected = r.get("connected")
                    want_relay = want_relay or r.get("configured", False)
                except ApiError:
                    r = {}
                if connected or not want_relay:
                    break
        except ApiError:
            pass
        time.sleep(1)
    if not ip:
        say(f"The board didn't join {ssid!r} in {a.timeout:g} s. Check the password, and that it is a 2.4 GHz network.")
        return 1
    if want_relay and not connected:
        say(f"On Wi-Fi at {ip}, but not connected to the relay in {a.timeout:g} s. "
            "The network may block outgoing port 8883; the Hive can still reach it on the LAN.")
        return 1
    say(f"On Wi-Fi at {ip}" + (", connected to the relay." if connected else "."))
    say(f"Board id: {did}")
    say("Done: move it to its charger (or leave it here) and leave it plugged in. The Hive takes it from here.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
