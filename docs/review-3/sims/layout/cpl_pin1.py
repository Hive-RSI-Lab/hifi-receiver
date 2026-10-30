#!/usr/bin/env python3
"""cpl_pin1.py - independent JLC placement check (review-3, LAYOUT).

Independent of hwde's cpl_verify.py (different code, same question), plus two
checks cpl_verify does not make:

 1. ROTATION/POSITION: for every CPL row, the part's LCSC EasyEDA model pads
    (cached JSON, PAD~ strings, 10 mil units, y down) are turned by the CPL
    rotation and placed at the CPL Mid X/Y; every model pad must land on the
    board pad with the same number (pad-centre error < 0.25 x the smaller pad
    dimension, min 0.1 mm). Pin 1 is reported separately.  Fail -> exit 1.
 2. PINOUT: for ICs/transistors/diodes, the board pad's net and KiCad
    pinfunction are listed against the LCSC symbol's pin NAME for the same
    pin number. A name that shares no token with the board's pinfunction is
    flagged for a human ("CHECK"), not failed automatically (vendors name
    pins differently: GND/VSS/EP...).
 3. MODEL PIN-1 MARK: the model's own silk pin-1 dot (CIRCLE on silk) must be
    nearer pad 1 than any other pad (the model is self-consistent) - else
    CHECK.

Usage: cpl_pin1.py BOARD.kicad_pcb CPL.csv CACHE_DIR [--parts parts.json]
CPL Mid Y is taken as KiCad's pos-file convention (y up => board y = -MidY);
both signs are tried and the one that fits the footprint origin is used.
"""
import csv, json, math, re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pcbgeo import load_board  # noqa: E402

U = 0.254


def model_pads(m):
    ox, oy = m["origin"]
    out = {}
    for s in m["pads"]:
        f = s.split("~")
        shape, x, y, w, h, layer, num = f[1], float(f[2]), float(f[3]), float(f[4]), float(f[5]), f[6], f[8]
        rot = float(f[11]) if len(f) > 11 and f[11] not in ("",) else 0.0
        if layer not in ("1", "11"):
            continue
        out.setdefault(num, []).append(((x - ox) * U, (y - oy) * U, w * U, h * U, rot))
    return out


def model_pin1_dot(m):
    ox, oy = m["origin"]
    dots = []
    for s in m.get("silk", []):
        f = s.split("~")
        if f[0] == "CIRCLE":
            dots.append(((float(f[1]) - ox) * U, (float(f[2]) - oy) * U, float(f[3]) * U))
    return dots


def pin_names(m):
    names = {}
    for s in m.get("pins", []):
        parts = s.split("^^")
        head = parts[0].split("~")
        num = head[3] if len(head) > 3 else None
        nm = None
        if len(parts) > 3:
            t = parts[3].split("~")
            if len(t) > 4:
                nm = t[4]
        if num:
            names[num] = nm
    return names


def rot(x, y, deg):
    r = math.radians(-deg)  # CCW on a y-down canvas
    return x * math.cos(r) - y * math.sin(r), x * math.sin(r) + y * math.cos(r)


def toks(s):
    return set(t for t in re.split(r"[^A-Z0-9]+", (s or "").upper()) if t)


def main():
    a = sys.argv[1:]
    if len(a) < 3:
        print(__doc__); return 2
    pcb, cplf, cache = a[0], a[1], Path(a[2])
    fps = load_board(pcb)["footprints"]
    rows = list(csv.DictReader(open(cplf)))
    fails, checks, nomodel = [], [], []
    report = []
    lcsc_of = {}
    for ref, fp in fps.items():
        lcsc_of[ref] = fp["props"].get("LCSC") or fp["props"].get("LCSC Part #") or fp["props"].get("JLCPCB Part #")
    if "--parts" in a:
        pj = json.load(open(a[a.index("--parts") + 1]))
        items = pj.get("parts", pj) if isinstance(pj, dict) else pj
        if isinstance(items, dict):
            items = [dict(v, ref=k) if isinstance(v, dict) else {} for k, v in items.items()]
        for it in items:
            if not isinstance(it, dict):
                continue
            refs = it.get("refs") or it.get("designators") or ([it["ref"]] if it.get("ref") else [])
            if isinstance(refs, str):
                refs = [r.strip() for r in refs.split(",")]
            l = it.get("lcsc") or it.get("LCSC")
            for r in refs:
                if l:
                    lcsc_of[r] = l
    for row in rows:
        ref = row["Designator"]
        fp = fps.get(ref)
        if not fp:
            fails.append((ref, "not on board")); continue
        l = lcsc_of.get(ref)
        mf = cache / f"{l}.json"
        if not l or not mf.exists():
            nomodel.append(ref); continue
        m = json.load(open(mf))
        mp = model_pads(m)
        cr = float(row["Rotation"])
        mx = float(row["Mid X"].replace("mm", ""))
        my = float(row["Mid Y"].replace("mm", ""))
        # choose the Y sign that matches the footprint origin
        cands = [(mx, -my), (mx, my)]
        px, py = min(cands, key=lambda c: math.hypot(c[0] - fp["x"], c[1] - fp["y"]))
        bp = {}
        for p in fp["pads"]:
            bp.setdefault(p["num"], []).append(p)
        errs = []
        for num, lst in mp.items():
            if num not in bp or not num:
                continue
            for (x, y, w, h, _r) in lst:
                rx, ry = rot(x, y, cr)
                gx, gy = px + rx, py + ry
                d = min(math.hypot(gx - b["x"], gy - b["y"]) for b in bp[num])
                tol = max(0.1, 0.25 * min(min(b["w"], b["h"]) for b in bp[num]))
                errs.append((num, d, tol))
        if len([e for e in errs]) < 2:
            checks.append((ref, "fewer than 2 shared pad numbers")); continue
        bad = [e for e in errs if e[1] > e[2]]
        p1 = [e for e in errs if e[0] == "1"]
        rec = dict(ref=ref, lcsc=l, cpl_rot=cr, n_pads=len(errs), max_err_mm=round(max(e[1] for e in errs), 3),
                   pin1_err_mm=round(p1[0][1], 3) if p1 else None, bad=[(e[0], round(e[1], 2)) for e in bad][:6])
        # pinout and pin-1 dot only for multi-pin active parts
        if ref[0] in "UQD" and len(mp) >= 3:
            names = pin_names(m)
            mism = []
            for num, lst in bp.items():
                b = lst[0]
                nm = names.get(num)
                pf = b.get("pinfunction") or ""
                if nm and pf and not (toks(nm) & toks(pf)) and toks(nm) and not (nm.isdigit()):
                    mism.append(f"{num}:{nm}|{pf}|{b.get('net')}")
            rec["pin_name_check"] = mism
            dots = model_pin1_dot(m)
            if dots and "1" in mp:
                nearest_ok = []
                for dx, dy, r in dots:
                    dn = sorted(((math.hypot(dx - x, dy - y), n) for n, lst in mp.items() for (x, y, *_r) in lst))
                    nearest_ok.append(dn[0][1])
                rec["model_dot_nearest_pad"] = nearest_ok
                if "1" not in nearest_ok and not any(n in ("", None) for n in nearest_ok):
                    checks.append((ref, f"model silk dot nearest pads {nearest_ok}, not 1"))
            if mism:
                checks.append((ref, "pin-name mismatch " + "; ".join(mism[:8])))
        report.append(rec)
        if bad:
            fails.append((ref, f"pads off model by > tol: {rec['bad']} at CPL rot {cr}"))
    out = dict(board=pcb, cpl=cplf, n=len(report), fails=fails, checks=checks, no_model=nomodel, parts=report)
    if "--out" in a:
        json.dump(out, open(a[a.index("--out") + 1], "w"), indent=1)
    print(f"{Path(cplf)}: {len(report)} parts fitted, {len(fails)} FAIL, {len(checks)} CHECK, {len(nomodel)} no model")
    for f in fails:
        print("  FAIL", *f)
    for c in checks:
        print("  CHECK", *c)
    if nomodel:
        print("  NO-MODEL", " ".join(nomodel))
    return 1 if (fails or nomodel) else 0


if __name__ == "__main__":
    sys.exit(main())
