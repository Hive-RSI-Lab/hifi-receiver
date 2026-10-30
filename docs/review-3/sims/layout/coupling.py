#!/usr/bin/env python3
"""coupling.py - victim trace length running near aggressor copper (review-3 LAYOUT).

coupling.py BOARD VICTIM_NET[,..] AGGR_REGEX [--d 1.0]
For each victim track sample (0.25 mm), finds aggressor copper (tracks, pads,
filled zones of nets matching AGGR_REGEX) on the same layer within --d mm, or
on the adjacent layer directly under/over (within --d/2 lateral). Prints the
coupled length per victim per aggressor. Exit 1 if any victim has > --max mm
(default 5) coupled to an aggressor.
"""
import math, re, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pcbgeo import load_board, point_in_poly

ADJ = {"F.Cu": ["In1.Cu"], "In1.Cu": ["F.Cu", "In2.Cu"], "In2.Cu": ["In1.Cu", "B.Cu"], "B.Cu": ["In2.Cu"]}
ADJ2 = {"F.Cu": ["B.Cu"], "B.Cu": ["F.Cu"]}


def segd(px, py, a, b):
    (x1, y1), (x2, y2) = a, b
    dx, dy = x2 - x1, y2 - y1
    L = dx * dx + dy * dy
    t = 0 if L == 0 else max(0, min(1, ((px - x1) * dx + (py - y1) * dy) / L))
    return math.hypot(px - x1 - t * dx, py - y1 - t * dy)


def main():
    a = sys.argv[1:]
    b = load_board(a[0]); victims = a[1].split(","); rx = re.compile(a[2])
    D = float(a[a.index("--d") + 1]) if "--d" in a else 1.0
    MX = float(a[a.index("--max") + 1]) if "--max" in a else 5.0
    adj = ADJ if any(l == "In1.Cu" for l in b["layers"]) else ADJ2
    ag_t = [t for t in b["tracks"] if t["net"] and rx.search(t["net"])]
    ag_p = [(p, f) for f in b["footprints"].values() for p in f["pads"] if p["net"] and rx.search(p["net"])]
    ag_z = []
    for z in b["zones"]:
        if z["net"] and rx.search(z["net"]):
            for lay, pts in z["filled"]:
                xs = [q[0] for q in pts]; ys = [q[1] for q in pts]
                ag_z.append((lay, min(xs), min(ys), max(xs), max(ys), pts, z["net"]))
    bad = 0
    for v in victims:
        acc = {}
        for t in b["tracks"]:
            if t["net"] != v:
                continue
            (x1, y1), (x2, y2) = t["s"], t["e"]
            n = max(1, int(math.hypot(x2 - x1, y2 - y1) / 0.25)); st = math.hypot(x2 - x1, y2 - y1) / n
            for i in range(n):
                x = x1 + (x2 - x1) * (i + .5) / n; y = y1 + (y2 - y1) * (i + .5) / n
                hit = set()
                for g in ag_t:
                    if g["layer"] == t["layer"] and segd(x, y, g["s"], g["e"]) - g["w"] / 2 < D:
                        hit.add((g["net"], "same"))
                    elif g["layer"] in adj.get(t["layer"], []) and segd(x, y, g["s"], g["e"]) - g["w"] / 2 < D / 2:
                        hit.add((g["net"], "adj"))
                for p, f in ag_p:
                    if math.hypot(x - p["x"], y - p["y"]) < max(p["w"], p["h"]) / 2 + D:
                        hit.add((p["net"], "pad"))
                for lay, X0, Y0, X1, Y1, pts, net in ag_z:
                    if (lay == t["layer"] or lay in adj.get(t["layer"], [])) and X0 <= x <= X1 and Y0 <= y <= Y1 and point_in_poly(x, y, pts):
                        hit.add((net, "zone-adj" if lay != t["layer"] else "zone-same"))
                for h in hit:
                    acc[h] = acc.get(h, 0) + st
        worst = max(acc.values()) if acc else 0
        flag = "FAIL" if worst > MX else "ok"
        bad += flag == "FAIL"
        print(f"{v:22s} {flag} " + "; ".join(f"{k[0]}({k[1]}) {l:.1f}mm" for k, l in sorted(acc.items(), key=lambda kv: -kv[1])[:6]))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
