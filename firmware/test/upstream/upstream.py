#!/usr/bin/env python3
"""Our patches on upstream squeezelite-esp32, as source a host test can compile.

    upstream.py files OUT PATH...        OUT/PATH = upstream's file at the
                                         pinned commit with patches/ applied
    upstream.py extract FILE NAME...     print the top-level C functions
                                         (and #defines) of that name

The upstream file comes from $HIFI_UPSTREAM (a git checkout that has the
pinned commit, such as a build tree from tools/fetch.sh), else from GitHub
at the pinned commit; either is kept in OUT/.pristine, so it is fetched once.
Exit 3 when no HIFI_UPSTREAM is set and GitHub can't be reached or answers
with an error other than a 404 (down, rate-limited): the caller skips the
upstream tests then. A HIFI_UPSTREAM without the pinned commit, or a 404
from GitHub (a wrong path or commit), is an error (exit 1), not a skip.

`extract` lets a test build upstream's own functions, as patched, against
stubs of the ESP-IDF calls they make, instead of a copy of them that would
drift from what the board runs.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

FW = Path(__file__).resolve().parents[2]


class Broken(Exception):
    """Upstream was reached but hasn't the pinned file: a failure, not a skip."""


def lock() -> dict[str, str]:
    out = {}
    for line in (FW / "upstream.lock").read_text().splitlines():
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def pristine(path: str, cache: Path) -> bytes | None:
    lk = lock()
    dest = cache / lk["commit"] / path
    if dest.is_file():
        return dest.read_bytes()
    data = None
    up = os.environ.get("HIFI_UPSTREAM")
    if up:
        r = subprocess.run(["git", "-C", up, "show", f"{lk['commit']}:{path}"], capture_output=True)
        if r.returncode:
            raise Broken(f"HIFI_UPSTREAM={up} has no {lk['commit']}:{path}: {r.stderr.decode().strip()}")
        data = r.stdout
    if data is None:
        slug = lk["url"].removeprefix("https://github.com/").removesuffix(".git")
        url = f"https://raw.githubusercontent.com/{slug}/{lk['commit']}/{path}"
        try:
            with urllib.request.urlopen(url, timeout=30) as resp:
                data = resp.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                raise Broken(f"GitHub has no {path} at {lk['commit']} ({e})") from e
            print(f"upstream.py: can't get {path} ({e}); set HIFI_UPSTREAM to a checkout", file=sys.stderr)
            return None
        except OSError as e:
            print(f"upstream.py: can't get {path} ({e}); set HIFI_UPSTREAM to a checkout", file=sys.stderr)
            return None
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    return data


def file_patches(path: str) -> list[str]:
    """The parts of patches/*.patch that change PATH, in order."""
    parts = []
    for p in sorted((FW / "patches").glob("*.patch")):
        for chunk in re.split(r"(?m)^(?=diff --git )", p.read_text()):
            if chunk.startswith(f"diff --git a/{path} "):
                parts.append(chunk)
    return parts


def files(out: Path, paths: list[str]) -> int:
    for path in paths:
        parts = file_patches(path)
        dest = out / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        if parts and "\n--- /dev/null\n" in parts[0]:
            # a file our patches add: there is no upstream copy, the patch makes it
            dest.unlink(missing_ok=True)
        else:
            try:
                data = pristine(path, out / ".pristine")
            except Broken as e:
                print(f"upstream.py: {e}", file=sys.stderr)
                return 1
            if data is None:
                return 3
            dest.write_bytes(data)
        for part in parts:
            r = subprocess.run(["patch", "-p1", "-s", "-N", "-d", str(out)], input=part.encode(),
                               capture_output=True)
            if r.returncode:
                print(f"upstream.py: a patch to {path} doesn't apply:\n{r.stdout.decode()}{r.stderr.decode()}",
                      file=sys.stderr)
                return 1
    return 0


def extract(src: str, names: list[str]) -> str:
    """Each `#define NAME` asked for, then each named top-level function's full definition."""
    defs, out = [], []
    for name in names:
        d = re.search(rf"(?m)^#define\s+{re.escape(name)}\b.*$", src)
        if d:
            defs.append(d.group(0))
            continue
        # a definition starts at column 0, has NAME( and its body brace after the ")"
        m = re.search(rf"(?m)^[A-Za-z_][^;{{}}()\n]*\b{re.escape(name)}\s*\([^;{{]*?\)\s*\{{", src)
        if not m:
            raise SystemExit(f"upstream.py: no function {name}")
        i, depth = m.end() - 1, 0
        while i < len(src):
            c = src[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    break
            elif c in "\"'":
                j = i + 1
                while j < len(src) and src[j] != c:
                    j += 2 if src[j] == "\\" else 1
                i = j
            elif src.startswith("/*", i):
                i = src.index("*/", i) + 1
            elif src.startswith("//", i):
                i = src.index("\n", i)
            i += 1
        out.append(f"#line {src.count(chr(10), 0, m.start()) + 1}\n{src[m.start():i + 1]}")
    return "\n".join(defs) + "\n\n" + "\n\n".join(out) + "\n"


def main(argv: list[str]) -> int:
    if len(argv) >= 3 and argv[0] == "files":
        return files(Path(argv[1]), argv[2:])
    if len(argv) >= 3 and argv[0] == "extract":
        sys.stdout.write(extract(Path(argv[1]).read_text(errors="replace"), argv[2:]))
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
