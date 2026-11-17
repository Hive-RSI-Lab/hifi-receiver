#!/usr/bin/env python3
"""Signs a receiver app image for updates from the LAN (security.md).

    sign_fw.py IMAGE [--key FILE]     appends the trailer in place (again: a no-op)
    sign_fw.py --check IMAGE PUBHEX   0 if IMAGE carries a good trailer for PUBHEX

The trailer (components/hifi/core/hifi_ota.h) is 72 bytes: b"HIFISIG1" and an
Ed25519 signature over b"hifi-fw-v1:" + the SHA-256, in lower-case hex, of
everything before it. ESP-IDF reads an app image's length from its own
headers, so USB flashing and the Hive's relay updates take a signed file as
they took an unsigned one; only port 80 asks for the trailer.

The key is the bench's Ed25519 seed (FILE, default ~/.config/h2/bench-dev-ed25519,
hex), the same key the board already trusts for relay commands. It is read
here and never printed. Without it the image is left unsigned and the board
refuses it from the LAN; it still installs over USB and from the Hive.
"""
import argparse
import hashlib
import os
import sys

MAGIC, CTX, LEN = b"HIFISIG1", b"hifi-fw-v1:", 72
DEFAULT_KEY = os.path.expanduser("~/.config/h2/bench-dev-ed25519")


def message(body):
    return CTX + hashlib.sha256(body).hexdigest().encode()


def split(data):
    if len(data) > LEN and data[-LEN:-LEN + 8] == MAGIC:
        return data[:-LEN], data[-64:]
    return data, None


def verify(pub, body, sig):
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    try:
        Ed25519PublicKey.from_public_bytes(pub).verify(sig, message(body))
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
    body, sig = split(data)

    if a.check:
        ok = sig is not None and a.pubhex and verify(bytes.fromhex(a.pubhex), body, sig)
        print(f"{os.path.basename(a.image)}: {'signed' if ok else 'not signed' if sig is None else 'bad signature'}")
        return 0 if ok else 1

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
        if verify(pub, body, sig):
            print(f"{os.path.basename(a.image)}: already signed")
            return 0
        print(f"sign_fw: {a.image} has a trailer that this key didn't sign: re-signing", file=sys.stderr)
    tmp = a.image + ".signing"
    with open(tmp, "wb") as f:
        f.write(body + MAGIC + key.sign(message(body)))
    os.replace(tmp, a.image)
    print(f"{os.path.basename(a.image)}: signed ({len(body)} + {LEN} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
