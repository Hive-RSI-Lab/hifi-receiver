#!/usr/bin/env python3
"""Regressions in upstream code too tied to its stack to compile on the host.

    test_up_source.py UPSTREAM_DIR    (the patched tree upstream.py made)

Each check reads the patched source for the fix and fails if a patch stops
carrying it (dropped, reverted, or upstream moved the code from under it).
Spotify (patch 0006): the person's partner couldn't take the speaker over
from their own account. Bluetooth pairing (0007): phones were asked to
compare a number on a speaker with no screen. Spotify login (0026): a
login that threw ended the Spotify task and rebooted the speaker. Wi-Fi events (0011): `diag`
missed the first connect and never had the boot's IP time (S8). Page scan (0036): with Wi-Fi
audio playing, a phone's connect sometimes went unanswered.
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
    return src[i:src.find(end, i + len(start))] if i >= 0 else ""


up = Path(sys.argv[1])
core = (up / "components/driver_bt/bt_app_core.c").read_text()
blob = (up / "components/spotify/cspot/src/LoginBlob.cpp").read_text()
shim = (up / "components/spotify/Shim.cpp").read_text()
net = (up / "components/wifi-manager/network_manager_handlers.c").read_text()
app = (up / "main/esp_app_main.c").read_text()
cmake = (up / "CMakeLists.txt").read_text()
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

# 0026: a login that throws (a MAC mismatch, a reset) fails like a refused
# one, so it falls back to ZeroConf instead of ending the task (a reboot)
auth = re.search(r"try\s*\{\s*ctx->config\.authData\s*=\s*ctx->session->authenticate\((?:blob|login)\);\s*\}\s*catch", shim)
check(auth is not None, "runTask's authenticate(...) is not inside a try")

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

# 0030: an IP change while a new image is on trial doesn't restart until the
# trial keeps it; with rollback on, a restart before then put the old image back
ipc = body(app, '"IP change, need to reboot"', "\n\t}\n")
held, restart = ipc.find("hifi_restart_after_trial("), ipc.find("esp_restart()")
check(held >= 0 and restart > held, "the IP-change reboot doesn't ask hifi_restart_after_trial first")
check(re.search(r"if\s*\(\s*!hifi_restart_after_trial\s*\|\|\s*!hifi_restart_after_trial\(", ipc) is not None,
      "the IP-change reboot doesn't skip esp_restart() while the restart is held")
relay = (Path(__file__).resolve().parents[2] / "components/hifi/esp/hifi_relay.c").read_text()
check(re.search(r"(?m)^bool hifi_restart_after_trial\(const char \*why\)\s*\{", relay) is not None,
      "components/hifi defines no hifi_restart_after_trial for patch 0030's hook")
check(re.search(r"\(valid = esp_ota_mark_app_valid_cancel_rollback\(\)\) != ESP_OK", relay) is not None,
      "the relay task doesn't check that the image was marked good before releasing a held restart")
kept = body(relay, "} else if (v == HIFI_TRIAL_GOOD) {", "} else if")
check("hifi_restart_release(" in kept and "esp_restart()" in kept and
      kept.find("wait_for_commit()") < kept.find("esp_restart()") and "wait_for_commit()" in kept,
      "the relay task doesn't run a held restart, settings committed, once the image is kept")
hook = body(relay, "bool hifi_restart_after_trial(const char *why) {", "\n}\n")
check("esp_ota_get_state_partition" not in hook and "portENTER_CRITICAL" in hook,
      "hifi_restart_after_trial reads otadata (blank while it is rewritten) instead of the hold in RAM")
# held, the connection carries on: mDNS and CONNECTED_BIT below still run
check("return" not in ipc[held:], "a held IP-change restart returns early and skips the rest of the connection")

# 0036: Bluedroid's bt component pages every 640 ms, not 1.28 s, so a phone's
# connect still lands when Wi-Fi coex takes a scan window; the window stays default
check(re.search(r"(?m)^target_compile_definitions\(__idf_bt PRIVATE BTM_DEFAULT_CONN_INTERVAL=0x0400\)", cmake)
      is not None, "the bt component isn't built with BTM_DEFAULT_CONN_INTERVAL=0x0400")
check("BTM_DEFAULT_CONN_WINDOW" not in cmake, "the page scan window is changed too (air time)")

print(f"{'up_source':<12} {checks} checks, {fails} failed")
sys.exit(1 if fails else 0)
