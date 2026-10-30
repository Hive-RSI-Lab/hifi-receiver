#!/usr/bin/env python3
"""Fill in LCSC prices in a board's parts.json, from a live search.

    python3 tools/price_parts.py boards/core/parts/parts.json [...] [--refresh]

For each part with an LCSC number and no price_breaks (all of them with
--refresh), runs hive-hwde's `hwde parts_search --query <C-number>` and
records its price, price_breaks, stock, basic flag and description. The
score prices a board from these committed figures (docs/checks.md), so it
doesn't drift with LCSC's live prices; run this when a board is sourced and
before an order, and commit the result.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

KEEP = ("price", "price_breaks", "stock", "basic", "description")


def hwde() -> str:
    found = os.environ.get("HWDE_BIN") or shutil.which("hwde")
    mods = Path(os.environ.get("HIVE_MODULES") or "/hive/modules")
    return found or str(mods / "hive-hwde" / "bin" / "hwde")


def search(lcsc: str) -> dict | None:
    r = subprocess.run([hwde(), "parts_search", "--query", lcsc, "--limit", "5"],
                       capture_output=True, text=True, stdin=subprocess.DEVNULL)
    try:
        got = json.loads(r.stdout)
    except ValueError:
        print(f"{lcsc}: no answer ({(r.stderr or r.stdout).strip()[-200:]})", file=sys.stderr)
        return None
    return next((x for x in got.get("results", []) if x.get("lcsc") == lcsc), None)


def main(argv: list[str]) -> int:
    refresh = "--refresh" in argv
    files = [Path(a) for a in argv if a != "--refresh"]
    if not files:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    docs = {f: json.loads(f.read_text()) for f in files}
    want = sorted({p["lcsc"] for d in docs.values() for p in d["parts"]
                   if p.get("lcsc") and (refresh or not p.get("price_breaks"))})
    with ThreadPoolExecutor(6) as pool:
        found = dict(zip(want, pool.map(search, want)))
    # Parallel first runs can race on hwde's own setup: retry one at a time.
    found.update({k: search(k) for k, v in found.items() if not v})
    missing = sorted(k for k, v in found.items() if not v)
    for f, d in docs.items():
        for p in d["parts"]:
            hit = found.get(p.get("lcsc"))
            if not hit:
                continue
            if "basic" in p and p["basic"] != hit.get("basic"):
                print(f"{f}: {p['lcsc']} ({p.get('mpn')}) basic {p['basic']} -> {hit.get('basic')}")
            p.update({k: hit[k] for k in KEEP if k in hit})
        f.write_text(json.dumps(d, indent=1, ensure_ascii=False) + "\n")
    print(f"priced {len(want) - len(missing)} of {len(want)} LCSC parts"
          + (f"; not found: {', '.join(missing)}" if missing else ""))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
