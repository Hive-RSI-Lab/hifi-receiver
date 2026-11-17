#!/usr/bin/env python3
"""tools/sign_fw.py's trailer is the one the board checks (L9): an image it
signs installs from the LAN on a board running that build or an older one,
not a newer one (build/test_remote --check-file, the firmware's own hifi_ota.c),
and the tool refuses to sign an image without a build number (run by
make -C firmware/test)."""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "tools", "sign_fw.py")
# make passes its BIN (tools/hwcheck.py builds into a temporary one)
CHECK = os.environ.get("TEST_REMOTE") or os.path.join(HERE, "build", "test_remote")
SEED = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"  # RFC 8032 test 1, as test_remote.c
PUB = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
BUILD = 1789000000


def image(tag=b"HIFI-BUILD:%d;" % BUILD, project=b"Squeezelite-ESP32"):
    d = bytearray(os.urandom(20000))
    d[0] = 0xE9
    d[32:36] = struct.pack("<I", 0xABCD5432)
    d[80:112] = project.ljust(32, b"\0")
    d = d.replace(b"HIFI-BUILD:", b"hifi-build:")  # no stray tag in the random bytes
    if tag:
        d[5000:5000 + len(tag)] = tag
    return bytes(d)


def run(*args):
    return subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True)


def board(path, running, want):
    r = subprocess.run([CHECK, "--check-file", path, str(running), str(want)], capture_output=True, text=True)
    assert r.returncode == 0, (running, want, r.stdout, r.stderr)
    return r.stdout.strip()


with tempfile.TemporaryDirectory() as t:
    key = os.path.join(t, "key")
    with open(key, "w") as f:
        f.write(SEED + "\n")
    img = os.path.join(t, "squeezelite.bin")
    body = image()
    with open(img, "wb") as f:
        f.write(body)

    r = run(img, "--key", key)
    assert r.returncode == 0 and "signed, build %d" % BUILD in r.stdout, r
    data = open(img, "rb").read()
    assert len(data) == len(body) + 80 and data[:len(body)] == body
    assert data[len(body):len(body) + 8] == b"HIFISIG2"
    assert struct.unpack("<Q", data[len(body) + 8:len(body) + 16])[0] == BUILD
    assert SEED not in r.stdout + r.stderr

    r = run(img, "--key", key)  # again: a no-op
    assert r.returncode == 0 and "already signed" in r.stdout and open(img, "rb").read() == data, r
    r = run("--check", img, PUB)
    assert r.returncode == 0 and r.stdout.strip().endswith("signed, build %d" % BUILD), r
    r = run("--check", img, "11" * 32)
    assert r.returncode == 1 and "bad signature" in r.stdout, r

    # the board: the same build or an older running one takes it; a newer one doesn't
    board(img, BUILD, 200)
    board(img, BUILD - 1, 200)
    assert "older build" in board(img, BUILD + 1, 403)
    assert "no build number" in board(img, 0, 403)

    # a v1 trailer is stripped and the image signed again, not stacked
    with open(img, "wb") as f:
        f.write(body + b"HIFISIG1" + b"\0" * 64)
    r = run(img, "--key", key)
    assert r.returncode == 0 and open(img, "rb").read() == data, r

    # no build number, a 0, or two tags: left unsigned, and the build fails
    for bad in (image(tag=None), image(tag=b"HIFI-BUILD:0;"), image(tag=b"HIFI-BUILD:5;HIFI-BUILD:6;")):
        with open(img, "wb") as f:
            f.write(bad)
        r = run(img, "--key", key)
        assert r.returncode == 1 and "left unsigned" in r.stderr and open(img, "rb").read() == bad, r
        assert "isn't signed" in board(img, BUILD, 403)

    # no key: unsigned, but not an error (a build on a machine without the bench)
    with open(img, "wb") as f:
        f.write(body)
    r = run(img, "--key", os.path.join(t, "none"))
    assert r.returncode == 0 and open(img, "rb").read() == body, r
    assert "isn't signed" in board(img, BUILD, 403)

print("test_sign_fw: ok")
