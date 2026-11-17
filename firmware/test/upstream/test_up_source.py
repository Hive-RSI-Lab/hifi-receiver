#!/usr/bin/env python3
"""Regressions in upstream code too tied to its stack to compile on the host.

    test_up_source.py UPSTREAM_DIR    (the patched tree upstream.py made)

Each check reads the patched source for the fix and fails if a patch stops
carrying it (dropped, reverted, or upstream moved the code from under it).
Spotify (patch 0006): the person's partner couldn't take the speaker over
from their own account. Bluetooth pairing (0007): phones were asked to
compare a number on a speaker with no screen.
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

print(f"{'up_source':<12} {checks} checks, {fails} failed")
sys.exit(1 if fails else 0)
