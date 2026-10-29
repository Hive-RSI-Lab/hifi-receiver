#!/usr/bin/env python3
"""Check every board's fab/BOM.csv against live JLC and LCSC stock.

    python3 tools/bom_stock.py [--boards N] [--json OUT] [BOARD ...]

BOARD is a folder under boards/ (lite-main, core) or a variant under it
(core/ultra). With none given, every boards/*/fab/BOM.csv and
boards/*/fab/*/BOM.csv is checked.

Each LCSC part is looked up at JLC's assembly parts search and at LCSC's
product page. JLC's stockCount is its whole stock; canPresaleNumber is that
less what open orders have claimed, and goes negative when they've claimed
more (USB4105-GF-A-120 C5184243 read 2230 and -372 on 10-29, and JLC's BOM
review showed it as 0). That free count is what an order is placed from; a
part JLC marks isBuyComponent=0 counts as 0 free whatever its stock (JLC
then swaps in its componentAlternativesCode, which may be out).
LCSC's stockNumber is what can be bought into a JLC parts inventory first
(basic parts read 0 there: JLC stocks them itself). A line needs Qty x N
parts for N boards. Its status is:

    ok     JLC has enough free
    thin   JLC hasn't, LCSC has: buy it into the inventory before ordering
    short  neither has enough

docs/bom-alternates.csv names alternates checked by hand (rating, pads) for
lines that came up thin or short, per board; each is printed beside its line
with its own live stock, so a stale one shows. docs/bom-stock.md has the
last run and what to do at order time. This is a network check: it
is not part of tools/hwcheck.py's test. Standard library only.
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BOARDS = ROOT / "boards"
ALTERNATES = ROOT / "docs" / "bom-alternates.csv"
JLC = "https://jlcpcb.com/api/overseas-pcb-order/v1/shoppingCart/smtGood/selectSmtComponentList"
LCSC = "https://wmsc.lcsc.com/ftps/wm/product/detail?productCode="
UA = {"User-Agent": "Mozilla/5.0", "Content-Type": "application/json"}


def fetch(url: str, body: dict | None = None, tries: int = 3) -> dict:
    data = json.dumps(body).encode() if body is not None else None
    for i in range(tries):
        try:
            req = urllib.request.Request(url, data=data, headers=UA)
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.load(r)
        except Exception:
            if i == tries - 1:
                raise
            time.sleep(2 * (i + 1))
    return {}


def jlc_search(keyword: str, page_size: int = 25) -> list[dict]:
    out = fetch(JLC, {"keyword": keyword, "currentPage": 1, "pageSize": page_size})
    return ((out.get("data") or {}).get("componentPageInfo") or {}).get("list") or []


def jlc(code: str) -> dict:
    hit = next((p for p in jlc_search(code, 5) if p.get("componentCode") == code), None)
    if not hit:
        return {"found": False, "stock": 0, "free": 0}
    stock = int(hit.get("stockCount") or 0)
    claim = hit.get("canPresaleNumber")
    free = stock if claim is None else min(stock, int(claim))
    if str(hit.get("isBuyComponent")) == "0":
        free = 0  # JLC won't buy or place it (CH340C C84681: "no longer manufacture")
    return {"found": True, "stock": stock, "free": max(0, free),
            "no_buy": hit.get("noBuyReason") if str(hit.get("isBuyComponent")) == "0" else None,
            "substitute": hit.get("componentAlternativesCode") or None,
            "library": hit.get("componentLibraryType"),
            "model": hit.get("componentModelEn"),
            "package": hit.get("componentSpecificationEn")}


def lcsc(code: str) -> dict:
    res = (fetch(LCSC + code) or {}).get("result") or {}
    if not res:
        return {"found": False, "stock": 0}
    return {"found": True, "stock": int(res.get("stockNumber") or 0),
            "model": res.get("productModel"),
            "discontinued": bool(res.get("isDiscontinued"))}


def bom_files(names: list[str]) -> list[tuple[str, Path]]:
    if names:
        return [(n, BOARDS / n.split("/")[0] / "fab" / "/".join(n.split("/")[1:]) / "BOM.csv")
                for n in names]
    found = sorted(BOARDS.glob("*/fab/BOM.csv")) + sorted(BOARDS.glob("*/fab/*/BOM.csv"))
    return [("/".join(p.relative_to(BOARDS).parts[:1] + p.relative_to(BOARDS).parts[2:-1]), p)
            for p in found]


def alternates() -> dict[str, list[dict]]:
    alts: dict[str, list[dict]] = {}
    if ALTERNATES.exists():
        for r in csv.DictReader(open(ALTERNATES)):
            alts.setdefault(r["LCSC"], []).append(r)
    return alts


def status(need: int, j: dict, l: dict) -> str:
    if j.get("no_buy"):
        return "short"
    if j["free"] >= need:
        return "ok"
    return "thin" if l["stock"] >= need else "short"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("boards", nargs="*")
    ap.add_argument("--boards", dest="n", type=int, default=100,
                    help="boards to build (default 100)")
    ap.add_argument("--json", help="write every line's figures here")
    a = ap.parse_args(argv)
    alts, seen, report, bad = alternates(), {}, [], 0
    for name, bom in bom_files(a.boards):
        if not bom.exists():
            print(f"{name}: no {bom.relative_to(ROOT)}", file=sys.stderr)
            return 2
        for r in csv.DictReader(open(bom)):
            code = r["LCSC Part #"].strip()
            refs = [x for x in r["Designator"].split(",") if x.strip()]
            need = len(refs) * a.n
            if not code:
                report.append({"board": name, "refs": r["Designator"], "lcsc": "", "status": "no-lcsc"})
                continue
            if code not in seen:
                seen[code] = (jlc(code), lcsc(code))
            j, l = seen[code]
            st = status(need, j, l)
            line = {"board": name, "refs": r["Designator"], "comment": r["Comment"], "lcsc": code,
                    "need": need, "jlc_stock": j["stock"], "jlc_free": j["free"],
                    "jlc_library": j.get("library"), "jlc_no_buy": j.get("no_buy"), "lcsc_stock": l["stock"], "status": st,
                    "alternates": [dict(x) for x in alts.get(code, [])
                                   if x["Boards"] == "*" or name in x["Boards"].split()]}
            for alt in line["alternates"]:
                if alt["Alternate"] not in seen:
                    seen[alt["Alternate"]] = (jlc(alt["Alternate"]), lcsc(alt["Alternate"]))
                aj, al = seen[alt["Alternate"]]
                alt.update(jlc_free=aj["free"], lcsc_stock=al["stock"], status=status(need, aj, al))
            report.append(line)
            if st != "ok":
                bad += st == "short"
                print(f"{st:5} {name:12} {r['Designator'][:28]:28} {code:10} {r['Comment'][:22]:22} "
                      f"need {need:5}  JLC {j['stock']} (free {j['free']})  LCSC {l['stock']}"
                      + (f"  not buyable: {j['no_buy']}, JLC swaps in {j['substitute']}" if j.get("no_buy") else ""))
                for alt in line["alternates"]:
                    print(f"      alternate {alt['Alternate']} {alt['MPN']} ({alt['Fit']}), {alt['status']}: "
                          f"JLC free {alt['jlc_free']}, LCSC {alt['lcsc_stock']}. {alt['Note']}")
    ok = sum(1 for x in report if x["status"] == "ok")
    print(f"{len(report)} lines on {len({x['board'] for x in report})} boards for {a.n} boards each: "
          f"{ok} ok, {sum(x['status'] == 'thin' for x in report)} thin, "
          f"{sum(x['status'] == 'short' for x in report)} short")
    if a.json:
        Path(a.json).write_text(json.dumps({"boards": a.n, "time": time.strftime("%Y-%m-%dT%H:%MZ", time.gmtime()),
                                            "lines": report}, indent=1) + "\n")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
