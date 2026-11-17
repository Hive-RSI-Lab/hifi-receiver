#!/usr/bin/env python3
"""Signs a receiver app image for updates from the LAN (security.md).

    sign_fw.py IMAGE [--key FILE]     appends the trailer in place (again: a no-op)
    sign_fw.py --check IMAGE PUBHEX   0 if IMAGE carries a good trailer for PUBHEX

The trailer (components/hifi/core/hifi_ota.h) is 80 bytes: b"HIFISIG2", the
image's build number (8 bytes, little-endian) and an Ed25519 signature over
b"hifi-fw-v2:<build>:" + the SHA-256, in lower-case hex, of everything before
it. The build number is the one tools/build.sh compiled in, read from the
image's "HIFI-BUILD:<n>;" tag; a board refuses a LAN update with a lower one
than it runs (L9). ESP-IDF reads an app image's length from its own headers,
so USB flashing and the Hive's relay updates take a signed file as they took
an unsigned one; only port 80 asks for the trailer.

The key is the bench's Ed25519 seed (FILE, default ~/.config/h2/bench-dev-ed25519,
hex), the same key the board already trusts for relay commands. It is read
here and never printed. Without it, or without a build number in the image,
the image is left unsigned and the board refuses it from the LAN; it still
installs over USB and from the Hive.
"""
import argparse
import hashlib
import os
import re
import struct
import sys

MAGIC, CTX, LEN = b"HIFISIG2", b"hifi-fw-v2:", 80
OLD = (b"HIFISIG1", 72)  # the first trailer, without a build number
DEFAULT_KEY = os.path.expanduser("~/.config/h2/bench-dev-ed25519")
TAG = re.compile(rb"HIFI-BUILD:(\d{1,20});")


def message(build, body):
    return CTX + str(build).encode() + b":" + hashlib.sha256(body).hexdigest().encode()


def split(data):
    """(body, build, signature) of a v2 trailer; (body, None, None) for none, or a v1 one stripped"""
    if len(data) > LEN and data[-LEN:-LEN + 8] == MAGIC:
        return data[:-LEN], struct.unpack("<Q", data[-LEN + 8:-64])[0], data[-64:]
    if len(data) > OLD[1] and data[-OLD[1]:-OLD[1] + 8] == OLD[0]:
        return data[:-OLD[1]], None, None
    return data, None, None


def build_of(body):
    """the build number compiled into the image, or an error"""
    tags = TAG.findall(body)
    if len(tags) != 1:
        return None, f"{len(tags)} build tags in the image, not one"
    n = int(tags[0])
    if not 0 < n < 1 << 64:
        return None, "the image was built without a build number (tools/build.sh sets one)"
    return n, None


def verify(pub, build, body, sig):
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    try:
        Ed25519PublicKey.from_public_bytes(pub).verify(sig, message(build, body))
        return True
    except InvalidSignature:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image")
    ap.add_argument("pubhex", nargs="?")
    ap.add_argument("--key", default=DEFAULT_KEY)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    data = open(a.image, "rb").read()
    body, tbuild, sig = split(data)
    name = os.path.basename(a.image)

    if a.check:
        ok = sig is not None and a.pubhex and verify(bytes.fromhex(a.pubhex), tbuild, body, sig)
        state = "signed" if ok else "not signed" if sig is None else "bad signature"
        print(f"{name}: {state}" + (f", build {tbuild}" if sig is not None else ""))
        return 0 if ok else 1

    build, why = build_of(body)
    if build is None:
        print(f"sign_fw: {why}: image left unsigned, the board takes it over USB and from the Hive only", file=sys.stderr)
        return 1
    if not os.access(a.key, os.R_OK):
        print(f"sign_fw: no signing key at {a.key}: image left unsigned, the board takes it over USB and from the Hive only", file=sys.stderr)
        return 0
    from cryptography.hazmat.primitives import serialization as s
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    with open(a.key) as f:
        seed = bytes.fromhex(f.read().strip())
    if len(seed) != 32:
        print(f"sign_fw: {a.key} is not a 32-byte Ed25519 seed", file=sys.stderr)
        return 1
    key = Ed25519PrivateKey.from_private_bytes(seed)
    del seed
    pub = key.public_key().public_bytes(s.Encoding.Raw, s.PublicFormat.Raw)
    if sig is not None:
        if tbuild == build and verify(pub, build, body, sig):
            print(f"{name}: already signed, build {build}")
            return 0
        print(f"sign_fw: {a.image} has a trailer that this key didn't sign for build {build}: re-signing", file=sys.stderr)
    tmp = a.image + ".signing"
    with open(tmp, "wb") as f:
        f.write(body + MAGIC + struct.pack("<Q", build) + key.sign(message(build, body)))
    os.replace(tmp, a.image)
    print(f"{name}: signed, build {build} ({len(body)} + {LEN} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
