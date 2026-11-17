#!/usr/bin/env python3
"""Regressions in upstream code too tied to its stack to compile on the host.

    test_up_source.py UPSTREAM_DIR    (the patched tree upstream.py made)

Each check reads the patched source for the fix and fails if a patch stops
carrying it (dropped, reverted, or upstream moved the code from under it).
Spotify (patch 0006): the person's partner couldn't take the speaker over
from their own account. Bluetooth pairing (0007): phones were asked to
compare a number on a speaker with no screen. Wi-Fi events (0011): `diag`
missed the first connect and never had the boot's IP time (S8).
"""
import re
import sys
from pathlib import Path

fails = checks = 0


def check(ok: bool, what: str) -> None:
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
        print(f"test_up_source: {what}", file=sys.stderr)


def body(src: str, start: str, end: str) -> str:
    i = src.find(start)
    return src[i:src.find(end, i)] if i >= 0 else ""


up = Path(sys.argv[1])
core = (up / "components/driver_bt/bt_app_core.c").read_text()
blob = (up / "components/spotify/cspot/src/LoginBlob.cpp").read_text()
shim = (up / "components/spotify/Shim.cpp").read_text()
net = (up / "components/wifi-manager/network_manager_handlers.c").read_text()
diag = (Path(__file__).resolve().parents[2] / "components/hifi/esp/hifi_diag_esp.c").read_text()

# 0007: just-works pairing, no number to compare
check(re.search(r"iocap\s*=\s*ESP_BT_IO_CAP_NONE\s*;", core) is not None, "Bluetooth iocap is not ESP_BT_IO_CAP_NONE")
check("ESP_BT_IO_CAP_IO;" not in core, "Bluetooth still sets ESP_BT_IO_CAP_IO")

# 0006: zeroconf info says anyone may log in, in both of its builders
avail = re.findall(r'"availability"\s*[,\]]\s*[=]?\s*([^;)]*)', blob)
check(len(avail) == 2, f"expected 2 availability fields in LoginBlob.cpp, found {len(avail)}")
check(all(a.strip() == '""' for a in avail), f'availability is not "" everywhere: {avail}')
check("this->username" not in "".join(avail), "availability still names the logged-in user")

# 0006: a new account on addUser ends the running session
after = body(shim, "loadZeroconfQuery(queryMap);", "catch")
check("sessionUser" in after and "!=" in after, "addUser no longer compares the new user with the session's")
check(re.search(r"cmdHandler\(CSPOT_DISC\);\s*state\s*=\s*ABORT;", after) is not None,
      "addUser no longer ends the session for a new account")
check(re.search(r"sessionUser\s*=\s*ctx->config\.username", shim) is not None,
      "the session's user is never recorded")
check(shim.count("sessionMutex") >= 3, "sessionUser is read or written without its lock")

# 0011: the receiver's Wi-Fi handlers go on as soon as the event loop exists,
# before the network state machine can start Wi-Fi or Ethernet
init = body(net, "NETWORK_INITIALIZING_STATE_entry_handler(state_machine_t* const State_Machine) {", "\n}")
loop, hook = init.find("esp_event_loop_create_default()"), init.find("hifi_net_loop_ready()")
check(loop >= 0 and hook > loop, "hifi_net_loop_ready is not called after the default event loop is made")
check(hook >= 0 and all(init.find(x, loop) < 0 or hook < init.find(x, loop)
                        for x in ("network_wifi_global_init", "esp_event_handler_register", "NETWORK_EXECUTE_CB")),
      "hifi_net_loop_ready is called after the network can start")
check(re.search(r"(?m)^void hifi_net_loop_ready\(void\)\s*\{", diag) is not None,
      "components/hifi defines no hifi_net_loop_ready for patch 0011's hook")

print(f"{'up_source':<12} {checks} checks, {fails} failed")
sys.exit(1 if fails else 0)
