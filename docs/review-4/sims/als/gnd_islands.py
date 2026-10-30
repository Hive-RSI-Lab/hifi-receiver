#!/usr/bin/env python3
"""gnd_islands.py BOARD.kicad_pcb [x0 y0 x1 y1]: GND pour fragments (filled polygons) per layer, joined by
GND vias and GND through-hole pads; lists every fragment group with its area and the number of
stitches it has; a fragment group not joined to the main GND (largest) is an island -> exit 1.
Also lists fragments whose bbox meets the window with their stitch count (the mute ECO's area)."""
import sys
from pathlib import Path
sys.path.insert(0, "/work/docs/review-3/sims/layout")
from pcbgeo import load_board, point_in_poly, poly_area
b = load_board(sys.argv[1])
win = [float(v) for v in sys.argv[2:6]] if len(sys.argv) >= 6 else None
frags = []
for z in b["zones"]:
    if z["net"] != "GND":
        continue
    for lay, pts in z["filled"]:
        if len(pts) > 2:
            frags.append(dict(layer=lay, pts=pts, area=abs(poly_area(pts))))
par = list(range(len(frags)))
def f(i):
    while par[i] != i:
        par[i] = par[par[i]]; i = par[i]
    return i
stitch = [0] * len(frags)
pts_tht = [(v["xy"][0], v["xy"][1]) for v in b["vias"] if v.get("net") == "GND"]
for ref, fp in b["footprints"].items():
    for p in fp["pads"]:
        if p.get("net") == "GND" and p["kind"] in ("thru_hole",):
            pts_tht.append((p["x"], p["y"]))
for (x, y) in pts_tht:
    hit = [i for i, fr in enumerate(frags) if point_in_poly(x, y, fr["pts"])]
    for i in hit:
        stitch[i] += 1
    for i in hit[1:]:
        par[f(i)] = f(hit[0])
groups = {}
for i in range(len(frags)):
    groups.setdefault(f(i), []).append(i)
main = max(groups, key=lambda g: sum(frags[i]["area"] for i in groups[g]))
bad = 0
for g, mem in groups.items():
    a = sum(frags[i]["area"] for i in mem)
    if g != main:
        bad += 1
        xs = [p[0] for i in mem for p in frags[i]["pts"]]; ys = [p[1] for i in mem for p in frags[i]["pts"]]
        print(f"ISLAND group: {len(mem)} frag(s), {a:.2f} mm2, bbox x {min(xs):.1f}-{max(xs):.1f} y {min(ys):.1f}-{max(ys):.1f}")
print(f"{len(frags)} GND fragments, {len(groups)} group(s); main {sum(frags[i]['area'] for i in groups[main]):.0f} mm2")
if win:
    for i, fr in enumerate(frags):
        xs = [p[0] for p in fr["pts"]]; ys = [p[1] for p in fr["pts"]]
        if max(xs) >= win[0] and min(xs) <= win[2] and max(ys) >= win[1] and min(ys) <= win[3] and fr["area"] < 2000:
            print(f"  window frag {fr['layer']} {fr['area']:.1f} mm2 bbox x {min(xs):.1f}-{max(xs):.1f} y {min(ys):.1f}-{max(ys):.1f} stitches {stitch[i]}")
sys.exit(1 if bad else 0)
