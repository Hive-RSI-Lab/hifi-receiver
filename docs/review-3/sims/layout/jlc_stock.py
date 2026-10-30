#!/usr/bin/env python3
"""jlc_stock.py - JLC assembly stock and library type for every BOM line (review-3 LAYOUT).

For each row of a JLC BOM.csv (Comment,Designator,Footprint,LCSC Part #) asks
JLC's component search (the same API the JLC BOM tool uses) for the LCSC
number and reports library type (base/expand), stock, and the quantity the
order needs (designator count x --boards, default 5, plus JLC's attrition of
~5 % or 2 parts minimum for passives).

Fail (exit 1): a line with stock < need, a part JLC does not list, or a part
whose JLC entry is a different MPN than the BOM comment (printed, CHECK only).
Results are cached to out/stock_<bom-stem>.json with the query date, so the
report says when stock was read.  --offline reads that cache.

Usage: jlc_stock.py BOM.csv [--boards 5] [--tag lite]
"""
import csv, datetime, json, sys, time, urllib.request
from pathlib import Path

API = "https://jlcpcb.com/api/overseas-pcb-order/v1/shoppingCart/smtGood/selectSmtComponentList"


def query(lcsc):
    req = urllib.request.Request(API, data=json.dumps({"keyword": lcsc, "currentPage": 1, "pageSize": 5}).encode(),
                                 headers={"content-type": "application/json", "user-agent": "Mozilla/5.0"})
    d = json.load(urllib.request.urlopen(req, timeout=30))
    for it in (d.get("data") or {}).get("componentPageInfo", {}).get("list") or []:
        if it.get("componentCode") == lcsc or lcsc in (it.get("lcscGoodsUrl") or ""):
            return dict(type=it.get("componentLibraryType"), stock=it.get("stockCount"),
                        mpn=it.get("erpComponentName") or it.get("componentModelEn"),
                        package=it.get("componentSpecificationEn"))
    return None


def main():
    a = sys.argv[1:]
    bom = Path(a[0])
    boards = int(a[a.index("--boards") + 1]) if "--boards" in a else 5
    tag = a[a.index("--tag") + 1] if "--tag" in a else bom.parent.name
    cache = Path(__file__).resolve().parent / "out" / f"stock_{tag}.json"
    off = "--offline" in a
    old = json.load(open(cache)) if cache.exists() else {"parts": {}}
    res = {"date": old.get("date") if off else datetime.date.today().isoformat(), "boards": boards, "parts": {}}
    fails, ext = [], []
    for row in csv.DictReader(open(bom)):
        l = row["LCSC Part #"].strip()
        n = len([r for r in row["Designator"].split(",") if r.strip()])
        need = n * boards
        need = int(need * 1.05) + (2 if row["Footprint"].startswith(("C0", "R0", "L0", "C_", "R_")) else 0)
        info = old["parts"].get(l) if off else None
        if not off:
            for _ in range(3):
                try:
                    info = query(l); break
                except Exception as e:  # network hiccup
                    info = {"error": str(e)}; time.sleep(2)
            time.sleep(0.3)
        res["parts"][l] = info
        st = (info or {}).get("stock")
        typ = (info or {}).get("type")
        flag = ""
        if not info or "error" in info:
            flag = "NOT LISTED / ERROR"; fails.append(l)
        elif st is None or st < need:
            flag = f"SHORT (need {need})"; fails.append(l)
        if typ and typ != "base":
            ext.append(l)
        print(f"{l:11s} {typ or '?':7s} stock={st!s:>8} need={need:4d} {row['Comment'][:22]:22s} {row['Designator'][:30]:30s} {flag}")
    cache.parent.mkdir(exist_ok=True)
    json.dump(res, open(cache, "w"), indent=1)
    print(f"{bom}: {len(res['parts'])} lines, {len(ext)} extended (JLC fee ~$3 each), {len(fails)} FAIL: {fails}  [stock read {res['date']}]")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
