#!/usr/bin/env python3
"""Regression: the Spotify zeroconf login is not changed under a login (patch 0028).

    test_up_blob.py UPSTREAM_DIR   (the patched tree upstream.py made)

Shim.cpp keeps the speaker's zeroconf login in one LoginBlob. The httpd
task writes a new account into it when a phone picks the speaker (the
zeroconf POST); the cspot task reads it to log in, and the session keeps it
to log in again. Nothing locked it, so a POST during a login changed the
username and token while the session read them: a torn std::string or
vector there is a use-after-free on the board. LoginBlob also set the
account type before it knew the blob parsed, so a refused login still
changed it. Patch 0028 puts the blob behind a lock, gives each session a
copy of its own, and sets nothing until the blob has parsed.

The test builds upstream's Shim.cpp and LoginBlob.cpp, as patched, against
the stand-ins in upstream/blob (fakes.h: the http server, bell's task, the
session, a Crypto that decodes a blob written in the clear), with
ThreadSanitizer, and runs blob/harness.cpp's two cases:

  keeps   a login whose auth data runs past the blob's end is refused and
          the account before it stays
  race    alice's login is on its way to the access point when bob's POST
          lands: no data race, and the session logs in as alice
  snap    bob's POST lands while the cspot task copies alice's login out of
          the blob for a session: no data race, and the session is bob's

It then builds the same two files with 0028 taken back out and checks that
keeps and race fail, and builds the patched Shim.cpp with the POST handler's
lock taken out and checks that snap fails, so the test is known to catch
each of them.

Skipped, loudly, when this compiler's ThreadSanitizer can't run here;
UPSTREAM_REQUIRED=1 makes that a failure.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FW = HERE.parents[1]
SPOT = "components/spotify"
CSPOT = f"{SPOT}/cspot"
PATCH = "0028-spotify-loginblob-race.patch"
# headers Shim.cpp and LoginBlob.cpp include that fakes.h stands in for
FAKED = ["Session.h", "PlainConnection.h", "ApResolve.h", "BellTask.h", "MDNSService.h", "TrackPlayer.h",
         "CSpotContext.h", "SpircHandler.h", "CentralAudioBuffer.h", "Logger.h", "Utils.h", "BellLogger.h",
         "esp_http_server.h", "platform_config.h", "nvs_utilities.h", "tools.h",
         "protobuf/authentication.pb.h"]
# upstream's own, used as they are
REAL = [f"{SPOT}/cspot_private.h", f"{SPOT}/cspot_sink.h", f"{CSPOT}/include/LoginBlob.h",
        f"{CSPOT}/include/ConstantParameters.h", f"{CSPOT}/bell/external/cJSON/cJSON.h"]
TSAN = "halt_on_error=1:exitcode=66:report_signal_unsafe=0"
# the POST handler's lock in the patched Shim.cpp, and what snap's
# before-check puts in its place: a lock nobody else takes
POST_LOCK = ("                std::unique_lock blobLock(blobMutex);\n"
             "                blob->loadZeroconfQuery(queryMap);\n")
NO_POST_LOCK = ("                std::mutex nobody;\n"
                "                std::unique_lock blobLock(nobody);\n"
                "                blob->loadZeroconfQuery(queryMap);\n")
fails = checks = 0


def check(ok: bool, what: str) -> None:
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
        print(f"test_up_blob: {what}", file=sys.stderr)


def run(cmd: list, **kw) -> None:
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        raise SystemExit(f"test_up_blob: {' '.join(map(str, cmd))[:300]}\n{r.stdout[-3000:]}{r.stderr[-3000:]}")


def includes(inc: Path, up: Path) -> None:
    shutil.rmtree(inc, ignore_errors=True)
    for h in FAKED:
        (inc / h).parent.mkdir(parents=True, exist_ok=True)
        (inc / h).write_text('#include "fakes.h"\n')
    (inc / "Crypto.h").write_text('#include "fake_crypto.h"\n')
    for h in REAL:
        shutil.copy(up / h, inc / Path(h).name)


def unpatched(up: Path, out: Path) -> dict[str, Path] | None:
    """Shim.cpp and LoginBlob.cpp as upstream.py made them, with 0028 reversed."""
    text = (FW / "patches" / PATCH).read_text()
    got = {}
    for chunk in re.split(r"(?m)^(?=diff --git )", text):
        m = re.match(r"diff --git a/(\S+) ", chunk)
        if not m:
            continue
        path = m.group(1)
        dest = out / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(up / path, dest)
        r = subprocess.run(["patch", "-p1", "-s", "-R", "--no-backup-if-mismatch", "-d", str(out)],
                           input=chunk, capture_output=True, text=True)
        if r.returncode:
            print(f"test_up_blob: {PATCH} doesn't come back out of {path} (a later patch changed the "
                  f"same lines?), so the before-check is skipped:\n{r.stdout}{r.stderr}", file=sys.stderr)
            return None
        got[Path(path).name] = dest
    return got


def build(name: str, shim: Path, blob: Path, inc: Path, cjson: Path, out: Path) -> Path:
    exe = out / name
    run(["g++", "-std=gnu++20", "-g", "-O1", "-fsanitize=thread", "-w", "-DBELL_ONLY_CJSON",
         '-DCLIENT_ID="test"', '-DCLIENT_SECRET="test"', "-I", inc, "-I", HERE / "blob",
         "-o", exe, HERE / "blob/harness.cpp", shim, blob, cjson, "-lpthread"])
    return exe


def case(exe: Path, what: str) -> tuple[int, str]:
    env = dict(os.environ, TSAN_OPTIONS=TSAN)
    try:
        r = subprocess.run([exe, what], capture_output=True, text=True, timeout=60, env=env)
    except subprocess.TimeoutExpired:
        return -1, "timed out"
    return r.returncode, (r.stdout + r.stderr).strip()


def tsan_works(out: Path) -> bool:
    src = out / "probe.cpp"
    src.write_text("#include <thread>\nint main(){int x=0;std::thread t([&]{x=1;});t.join();return x-1;}\n")
    r = subprocess.run(["g++", "-fsanitize=thread", "-o", out / "probe", src], capture_output=True)
    return r.returncode == 0 and subprocess.run([out / "probe"], capture_output=True).returncode == 0


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 64
    up = Path(argv[0]).resolve()
    out = up / "blob"
    out.mkdir(parents=True, exist_ok=True)
    if not tsan_works(out):
        print("SKIPPED test_up_blob: ThreadSanitizer doesn't build or run here", file=sys.stderr)
        return 1 if os.environ.get("UPSTREAM_REQUIRED") == "1" else 0
    inc = out / "inc"
    includes(inc, up)
    cjson = out / "cJSON.o"
    run(["gcc", "-g", "-O1", "-fsanitize=thread", "-w", "-c", "-o", cjson,
         up / CSPOT / "bell/external/cJSON/cJSON.c"])

    exe = build("patched", up / SPOT / "Shim.cpp", up / CSPOT / "src/LoginBlob.cpp", inc, cjson, out)
    rc, log = case(exe, "keeps")
    check(rc == 0, f"keeps (patched) exit {rc}:\n{log}")
    # the race cases three times each: each must be clean
    for what in ("race", "snap"):
        for n in range(3):
            rc, log = case(exe, what)
            check(rc == 0, f"{what} (patched, run {n + 1}) exit {rc}:\n{log[-4000:]}")

    shim = (up / SPOT / "Shim.cpp").read_text()
    check(shim.count(POST_LOCK) == 1, "the POST handler's lock isn't where this test looks for it (Shim.cpp changed?)")
    if shim.count(POST_LOCK) == 1:
        nolock = out / "src-nolock" / "Shim.cpp"
        nolock.parent.mkdir(parents=True, exist_ok=True)
        nolock.write_text(shim.replace(POST_LOCK, NO_POST_LOCK))
        exe = build("nolock", nolock, up / CSPOT / "src/LoginBlob.cpp", inc, cjson, out)
        rc, log = case(exe, "snap")
        check(rc == 66 and "data race" in log,
              f"snap without the POST handler's lock should find the race (exit 66), got {rc}:\n{log[-2000:]}")

    before = unpatched(up, out / "src-unpatched")
    if before:
        exe = build("unpatched", before["Shim.cpp"], before["LoginBlob.cpp"], inc, cjson, out)
        rc, log = case(exe, "keeps")
        check(rc == 1, f"keeps without {PATCH} should fail (exit 1), got {rc}:\n{log}")
        rc, log = case(exe, "race")
        check(rc in (2, 66) and ("data race" in log or "logged in as" in log),
              f"race without {PATCH} should find the race (exit 66 or 2), got {rc}:\n{log[-2000:]}")
    print(f"up_blob      {checks} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
