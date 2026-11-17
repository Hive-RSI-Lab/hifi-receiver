#!/usr/bin/env python3
"""Regression: Spotify's TLS checks who it is talking to (patch 0023).

    test_up_tls.py UPSTREAM_DIR [--tls FILE]   (the patched tree upstream.py made)

cspot opens every Spotify connection (apresolve, the access point's login,
spclient, the audio CDN) through bell::TLSSocket. Bell verifies the server
only once its own X509Bundle has been initialised, and nothing on the board
initialises it, so the board took any certificate: anyone on the path could
read the Spotify tokens. Patch 0023 makes the socket verify every server on
the board against ESP-IDF's certificate bundle, as the relay and the updater
do.

The test builds upstream's TLSSocket.cpp, as patched and as the board
compiles it (ESP_PLATFORM), against the board's mbedTLS: IDF 4.4.8's
mbedtls component with our idf-patches/mbedtls-*.patch applied, as
tools/fetch.sh puts it in the tree (its x509_crt.c, its esp_crt_bundle.c and
its bundle, cacrt_all.pem), plus a test CA made here. A Python TLS server
on 127.0.0.1 then shows it three certificates: the test CA's for localhost (must connect), a self-signed one for localhost
(must be refused), and the test CA's again under a name it isn't for
(must be refused). --tls FILE builds FILE in place of the patched socket:
with upstream's pristine TLSSocket.cpp the two bad cases connect, which is
the bug. HIFI_TLS_LIVE=1 adds apresolve.spotify.com:443 (must connect).

Skipped, loudly, without an ESP-IDF v4.4.8 (IDF_PATH, ~/esp/esp-idf-v4.4.8,
or the fw tool's copy); UPSTREAM_REQUIRED=1 makes that a failure.
"""
from __future__ import annotations

import hashlib
import os
import shutil
import socket
import ssl
import subprocess
import sys
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
BELL = "components/spotify/cspot/bell/main"
fails = checks = 0


def check(ok: bool, what: str) -> None:
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
        print(f"test_up_tls: {what}", file=sys.stderr)


def run(cmd: list, **kw) -> None:
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        raise SystemExit(f"test_up_tls: {' '.join(map(str, cmd))[:300]}\n{r.stdout[-2000:]}{r.stderr[-2000:]}")


def find_idf() -> Path | None:
    cands = [os.environ.get("IDF_PATH"), Path.home() / "esp/esp-idf-v4.4.8"]
    fw = shutil.which("fw") or str(Path.home() / "modules/fw/bin/fw")
    if os.access(fw, os.X_OK):
        r = subprocess.run([fw, "env", "."], capture_output=True, text=True, cwd=HERE.parents[1])
        for word in r.stdout.replace(";", " ").split():
            if word.startswith(("IDF_PATH=", "export IDF_PATH=")):
                cands.append(word.split("=", 1)[1].strip("'\""))
    cands.append(Path.home() / ".cache/fw/esp-idf-v4.4.8")
    for c in cands:
        if c and (Path(c) / "components/mbedtls/esp_crt_bundle/esp_crt_bundle.c").is_file():
            return Path(c)
    return None


def board_mbedtls(idf: Path, out: Path) -> Path:
    """IDF's mbedtls component as the board builds it: copied out of the IDF
    and our idf-patches/mbedtls-*.patch applied (tools/fetch.sh does the same).
    Made again when the patches change."""
    pats = sorted((HERE.parents[1] / "idf-patches").glob("mbedtls-*.patch"))
    stamp = hashlib.sha256(b"".join(p.name.encode() + p.read_bytes() for p in pats)).hexdigest()
    comp = out / "components/mbedtls"
    if (out / "stamp").is_file() and (out / "stamp").read_text() == stamp:
        return comp
    shutil.rmtree(out, ignore_errors=True)
    shutil.copytree(idf / "components/mbedtls", comp, ignore=shutil.ignore_patterns(".git"))
    for p in pats:
        run(["patch", "-p1", "-s", "--no-backup-if-mismatch", "-d", out, "-i", p])
    (out / "stamp").write_text(stamp)
    return comp


def mbedtls(comp: Path, out: Path) -> Path:
    """The board's mbedTLS 2.28, built for the host once per copy (out/build/library/*.a)."""
    lib = out / "build/library"
    tag = out / "built-from"
    if (lib / "libmbedtls.a").is_file() and tag.is_file() and tag.read_text() == (comp.parent.parent / "stamp").read_text():
        return lib
    shutil.rmtree(out / "build", ignore_errors=True)
    src = comp / "mbedtls"
    run(["cmake", "-S", src, "-B", out / "build", "-DENABLE_TESTING=OFF", "-DENABLE_PROGRAMS=OFF",
         "-DCMAKE_BUILD_TYPE=Release", "-DUSE_SHARED_MBEDTLS_LIBRARY=OFF", "-DMBEDTLS_FATAL_WARNINGS=OFF",
         # mbedTLS 2.28 asks for CMake 2.8.12; CMake 4 refuses that unless told
         "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"])
    run(["cmake", "--build", out / "build", "-j", str(min(8, os.cpu_count() or 1)),
         "--target", "mbedtls", "mbedx509", "mbedcrypto"])
    tag.write_text((comp.parent.parent / "stamp").read_text())
    return lib


def certs(d: Path) -> None:
    """A test CA, its leaf for localhost, and a self-signed localhost leaf."""
    if (d / "self.pem").is_file():
        return
    d.mkdir(parents=True, exist_ok=True)
    o = ["openssl"]
    run(o + ["req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-days", "3650",
             "-subj", "/CN=hifi test CA", "-keyout", d / "ca.key", "-out", d / "ca.pem",
             "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign"])
    run(o + ["req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes",
             "-subj", "/CN=localhost", "-keyout", d / "good.key", "-out", d / "good.csr"])
    (d / "ext").write_text("subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n")
    run(o + ["x509", "-req", "-in", d / "good.csr", "-CA", d / "ca.pem", "-CAkey", d / "ca.key",
             "-CAcreateserial", "-days", "3650", "-extfile", d / "ext", "-out", d / "good.pem"])
    run(o + ["req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-days", "3650",
             "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
             "-keyout", d / "self.key", "-out", d / "self.pem"])


def bundle(comp: Path, ca: Path, d: Path) -> Path:
    """The board's bundle (its cacrt_all.pem, as CERTIFICATE_BUNDLE_DEFAULT_FULL) plus the test CA."""
    out = d / "x509_crt_bundle"
    if out.is_file() and (d / "built-from").is_file() and (d / "built-from").read_text() == (comp.parent.parent / "stamp").read_text():
        return out
    d.mkdir(parents=True, exist_ok=True)
    gen = comp / "esp_crt_bundle"
    run([sys.executable, gen / "gen_crt_bundle.py", "--quiet", "--input", gen / "cacrt_all.pem", ca], cwd=d)
    (d / "built-from").write_text((comp.parent.parent / "stamp").read_text())
    return out


def build(comp: Path, up: Path, tls: Path, lib: Path, bun: Path, exe: Path) -> None:
    bell = up / BELL
    inc = ["-I", HERE / "tls", "-I", bell / "io/include", "-I", bell / "utilities/include",
           "-I", comp / "mbedtls/include", "-I", comp / "esp_crt_bundle/include"]
    san = ["-g", "-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    o = exe.parent / (exe.name + ".o")
    o.mkdir(parents=True, exist_ok=True)
    cxx, cc = os.environ.get("CXX", "c++"), os.environ.get("CC", "cc")
    objs = []
    for src, extra in [(tls, ["-DESP_PLATFORM"]), (bell / "io/X509Bundle.cpp", []),
                       (bell / "utilities/BellLogger.cpp", []), (HERE / "tls/client.cpp", [])]:
        obj = o / (src.stem + ".o")
        run([cxx, "-std=c++17", "-c", *san, *inc, *extra, src, "-o", obj])
        objs.append(obj)
    obj = o / "esp_crt_bundle.o"
    run([cc, "-std=gnu11", "-c", *san, *inc, comp / "esp_crt_bundle/esp_crt_bundle.c", "-o", obj])
    objs.append(obj)
    obj = o / "bundle.o"
    run([cc, "-c", f'-DBUNDLE="{bun}"', HERE / "tls/bundle.S", "-o", obj])
    objs.append(obj)
    run([cxx, *san, *objs, lib / "libmbedtls.a", lib / "libmbedx509.a", lib / "libmbedcrypto.a", "-o", exe])


class Server:
    """TLS on 127.0.0.1 (and ::1 when there is one): each connection gets CERT and a greeting."""

    def __init__(self, cert: Path, key: Path):
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(cert, key)
        self.sock = socket.create_server(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.socks = [self.sock]
        try:
            self.socks.append(socket.create_server(("::1", self.port), family=socket.AF_INET6))
        except OSError:
            pass  # no IPv6 loopback, or the port is taken there: the client falls through to IPv4
        for s in self.socks:
            threading.Thread(target=self.serve, args=(s,), daemon=True).start()

    def serve(self, ls: socket.socket) -> None:
        while True:
            try:
                c, _ = ls.accept()
            except OSError:
                return
            c.settimeout(10)
            try:
                with self.ctx.wrap_socket(c, server_side=True) as t:
                    t.sendall(b"hello")
            except (OSError, ssl.SSLError):
                pass  # the client hung up on our certificate: that's the refused case
            finally:
                c.close()

    def close(self) -> None:
        for s in self.socks:
            s.close()


def connects(exe: Path, host: str, port: int, greet: bool = True) -> bool:
    r = subprocess.run([exe, host, str(port)] + (["greet"] if greet else []), capture_output=True, text=True,
                       timeout=60)
    if r.returncode not in (0, 1):
        raise SystemExit(f"test_up_tls: client {host}:{port} exit {r.returncode}\n{r.stdout}{r.stderr[-3000:]}")
    return r.returncode == 0 and (not greet or "hello" in r.stdout)


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__, file=sys.stderr)
        return 2
    up = Path(args[0]).resolve()  # the bundle generator runs in its own directory
    tls = Path(args[args.index("--tls") + 1]).resolve() if "--tls" in args else up / BELL / "io/TLSSocket.cpp"
    idf = find_idf()
    if not idf or not all(shutil.which(t) for t in ("cmake", "openssl", "patch")):
        print("SKIPPED test_up_tls: needs ESP-IDF v4.4.8 (IDF_PATH), cmake, openssl and patch")
        return 1 if os.environ.get("UPSTREAM_REQUIRED", "0") != "0" else 0
    work = up / "tls"
    comp = board_mbedtls(idf, work / "idf")
    lib = mbedtls(comp, work / "mbedtls")
    certs(work / "certs")
    bun = bundle(comp, work / "certs/ca.pem", work / "bundle")
    exe = work / ("client" if "--tls" not in args else "client-other")
    build(comp, up, tls, lib, bun, exe)

    c = work / "certs"
    good, selfs = Server(c / "good.pem", c / "good.key"), Server(c / "self.pem", c / "self.key")
    try:
        check(connects(exe, "localhost", good.port), "a server the bundle vouches for was refused")
        check(not connects(exe, "localhost", selfs.port), "a self-signed certificate was taken")
        check(not connects(exe, "127.0.0.1", good.port), "a certificate for another name was taken")
    finally:
        good.close()
        selfs.close()
    if os.environ.get("HIFI_TLS_LIVE"):
        check(connects(exe, "apresolve.spotify.com", 443, greet=False), "apresolve.spotify.com was refused")
    print(f"test_up_tls: {checks - fails}/{checks} passed ({tls.name}{' live' if os.environ.get('HIFI_TLS_LIVE') else ''})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
