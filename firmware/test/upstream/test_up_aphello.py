#!/usr/bin/env python3
"""Regression: Spotify's login checks the access point's key is Spotify's (patch 0025).

    test_up_aphello.py UPSTREAM_DIR [--auth FILE]   (the patched tree upstream.py made)

cspot logs in to a Spotify access point over a plain TCP socket: a
Diffie-Hellman exchange, then the credentials under the shared key. The
access point signs its half (gs) with Spotify's RSA key; upstream never
checked that signature, so anyone on the path could answer in Spotify's place
and read the credentials the board logs in with. Patch 0025 checks it, as
librespot does, and refuses the access point otherwise.

The test builds upstream's AuthChallenges.cpp as patched, with bell's
Crypto.cpp, NanoPBHelper.cpp and aes.c, nanopb, and keyexchange/authentication
generated from the patched .proto/.options by bell's own nanopb generator,
against the board's mbedTLS (as test_up_tls.py builds it: IDF 4.4.8's with
idf-patches/mbedtls-*.patch). It then hands solveApHello a real access
point's answer (aphello/apresponse.bin, recorded from ap-guc3 with
aphello/ap_record.py; only the server's public half, no credentials), which
must be taken, and that answer with another key under the real signature, an
altered signature, a short one, none, and no key exchange at all, each of
which must be refused. --auth FILE builds FILE in place of the patched
AuthChallenges.cpp: upstream's pristine one takes all of them, which is the bug.

Skipped, loudly, without an ESP-IDF v4.4.8, cmake, patch, a C++ compiler, or a
Python that can run the nanopb generator (protobuf and grpc_tools: this
Python, or the fw tool's IDF Python with its extra packages);
UPSTREAM_REQUIRED=1 makes that a failure.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_up_tls import board_mbedtls, find_idf, mbedtls  # noqa: E402

CSPOT = "components/spotify/cspot"
BELL = CSPOT + "/bell"
PROTOS = ("keyexchange", "authentication")
# expected outcome per case of aphello.cpp
WANT = {"real": "took", "reencoded": "took", "other-key": "refused", "bad-signature": "refused",
        "short-signature": "refused", "no-signature": "refused", "no-key-exchange": "refused",
        "restored": "took"}


def skip(why: str) -> int:
    print(f"SKIPPED test_up_aphello: {why}")
    return 1 if os.environ.get("UPSTREAM_REQUIRED", "0") != "0" else 0


def run(cmd: list, **kw) -> subprocess.CompletedProcess:
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        raise SystemExit(f"test_up_aphello: {' '.join(map(str, cmd))[:300]}\n{r.stdout[-2000:]}{r.stderr[-3000:]}")
    return r


def generator_python() -> tuple[str, dict] | None:
    """A Python that can import what bell's nanopb generator needs."""
    probe = "import google.protobuf, grpc_tools.protoc, pkg_resources"
    cands = [(sys.executable, {})]
    fw = Path.home() / ".cache/fw"
    extra = os.pathsep.join(str(p) for p in sorted((fw / "pyextra").glob("*")) if p.is_dir())
    for py in sorted((fw / "espressif/python_env").glob("*/bin/python")):
        cands.append((str(py), {"PYTHONPATH": extra} if extra else {}))
    for py, env in cands:
        e = dict(os.environ, **env)
        if subprocess.run([py, "-c", probe], capture_output=True, env=e).returncode == 0:
            return py, e
    return None


def generate(up: Path, gen: tuple[str, dict], out: Path) -> None:
    """keyexchange/authentication .pb.c/.pb.h as the board's build makes them."""
    py, env = gen
    out.mkdir(parents=True, exist_ok=True)
    tool = up / BELL / "external/nanopb/generator/nanopb_generator.py"
    for p in PROTOS:
        run([py, tool, "-I", ".", "-D", out, f"protobuf/{p}.proto"], cwd=up / CSPOT, env=env)
        if not (out / f"protobuf/{p}.pb.c").is_file():
            raise SystemExit(f"test_up_aphello: the generator made no {p}.pb.c in {out}")


def build(up: Path, auth: Path, comp: Path, lib: Path, gen: Path, exe: Path) -> None:
    cxx, cc = shutil.which("c++") or shutil.which("g++"), shutil.which("cc") or shutil.which("gcc")
    util, npb = up / BELL / "main/utilities", up / BELL / "external/nanopb"
    defs = ["-DPB_ENABLE_MALLOC", "-DPB_FIELD_32BIT", "-DPB_MESSAGE_NESTING_MAX=10"]  # cspot's CMakeLists
    inc = ["-I", up / CSPOT / "include", "-I", util / "include", "-I", npb, "-I", gen,
           "-I", comp / "mbedtls/include"]
    san = ["-g", "-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    objs = []
    srcs = [(auth, cxx), (HERE / "aphello/aphello.cpp", cxx), (util / "Crypto.cpp", cxx),
            (util / "NanoPBHelper.cpp", cxx), (util / "aes.c", cc)]
    srcs += [(npb / f"pb_{n}.c", cc) for n in ("common", "decode", "encode")]
    srcs += [(gen / f"protobuf/{p}.pb.c", cc) for p in PROTOS]
    for i, (src, comp_) in enumerate(srcs):
        o = exe.parent / f"{exe.name}-{i}-{src.stem}.o"
        std = ["-std=gnu++20"] if comp_ == cxx else ["-std=gnu11"]
        run([comp_, *std, *san, *defs, *inc, "-c", src, "-o", o])
        objs.append(o)
    run([cxx, *san, *objs, "-o", exe, "-L", lib, "-lmbedtls", "-lmbedx509", "-lmbedcrypto"])


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__, file=sys.stderr)
        return 2
    up = Path(args[0]).resolve()
    auth = Path(args[args.index("--auth") + 1]).resolve() if "--auth" in args else up / CSPOT / "src/AuthChallenges.cpp"
    idf = find_idf()
    if not idf or not all(shutil.which(t) for t in ("cmake", "patch")):
        return skip("needs ESP-IDF v4.4.8 (IDF_PATH), cmake and patch")
    if not (shutil.which("c++") or shutil.which("g++")):
        return skip("needs a C++ compiler")
    gen = generator_python()
    if not gen:
        return skip("needs a Python with protobuf and grpc_tools for nanopb's generator")
    tls = up / "tls"  # test_up_tls.py's mbedTLS, shared
    comp = board_mbedtls(idf, tls / "idf")
    lib = mbedtls(comp, tls / "mbedtls")
    work = up / "aphello"
    shutil.rmtree(work / "gen", ignore_errors=True)
    generate(up, gen, work / "gen")
    exe = work / ("aphello" if "--auth" not in args else "aphello-other")
    build(up, auth, comp, lib, work / "gen", exe)

    r = subprocess.run([exe, HERE / "aphello/apresponse.bin"], capture_output=True, text=True, timeout=120)
    if r.returncode:
        raise SystemExit(f"test_up_aphello: aphello exit {r.returncode}\n{r.stdout}{r.stderr[-3000:]}")
    got = {ln.split()[0]: ln.split()[1] for ln in r.stdout.splitlines() if len(ln.split()) >= 2}
    fails = 0
    for case, want in WANT.items():
        if got.get(case) != want:
            fails += 1
            print(f"test_up_aphello: {case}: want {want}, got {got.get(case, 'nothing')}", file=sys.stderr)
    print(f"test_up_aphello: {len(WANT) - fails}/{len(WANT)} passed ({auth.name})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
