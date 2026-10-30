#!/usr/bin/env python3
"""cap_return.py BOARD.kicad_pcb [MAX_MM]  (review-4 BAN; $HWDE_PY: shapely + numpy)

Output-filter cap return: the shortest path through GND copper (filled zones, GND tracks and pads on
F.Cu and B.Cu, joined at GND vias / through-hole pads) from each of C68-C71's GND pad to U8's nearest
GND pin (22, 25, 28, or the EP) -- the path the 400 kHz common-mode ripple (~1.3 A p-p per side in BD
mode, 21 V / 10 uH) takes back to PGND. Also the forward path (U8 OUT pin -> L pad 1 -> L pad 2 ->
C pad 1) straight-line length, and the loop's enclosed-area estimate (shoelace over the forward
polyline + the GND path). Grid 0.1 mm. Exits 1 if any GND path is longer than MAX_MM (default 30).
Run it on a refilled copy (kicad-cli pcb drc --refill-zones --save-board on /tmp).
"""
import heapq
import math
import sys

import numpy as np
sys.path.insert(0, "/work/docs/review-3/sims/layout")
from pcbgeo import load_board  # noqa: E402
from shapely.geometry import LineString, Point, Polygon, box  # noqa: E402
from shapely import affinity  # noqa: E402
from shapely.ops import unary_union  # noqa: E402
from shapely.prepared import prep  # noqa: E402

b = load_board(sys.argv[1])
ARGS = [a for a in sys.argv[1:] if a != "-v"]
LIM = float(ARGS[1]) if len(ARGS) > 1 else 30.0
fp = b["footprints"]
G = 0.1
WX0, WY0, WX1, WY1 = 75.0, 60.0, 130.0, 112.0      # window: sockets to U8
nx, ny = int((WX1 - WX0) / G), int((WY1 - WY0) / G)

shapes = {"F.Cu": [], "B.Cu": []}
for z in b["zones"]:
    if z["net"] == "GND":
        for lay, pts in z["filled"]:
            if lay in shapes and len(pts) > 2:
                shapes[lay].append(Polygon(pts).buffer(0))
for t in b["tracks"]:
    if t["net"] == "GND" and t["layer"] in shapes:
        shapes[t["layer"]].append(LineString([t["s"], t["e"]]).buffer(t["w"] / 2))
links = []
for v in b["vias"]:
    if v["net"] == "GND":
        links.append(v["xy"])
        for lay in shapes:
            shapes[lay].append(Point(v["xy"]).buffer(v["size"] / 2))
for ref, f in fp.items():
    for p in f["pads"]:
        if p["net"] != "GND":
            continue
        g = affinity.rotate(box(p["x"] - p["w"] / 2, p["y"] - p["h"] / 2, p["x"] + p["w"] / 2, p["y"] + p["h"] / 2),
                            -f["rot"], origin=(p["x"], p["y"]))
        lays = ["F.Cu", "B.Cu"] if p["kind"] == "thru_hole" else [l for l in p["layers"] if l in shapes]
        for lay in lays:
            shapes[lay].append(g)
        if p["kind"] == "thru_hole":
            links.append((p["x"], p["y"]))

grid = {}
xsc = WX0 + (np.arange(nx) + 0.5) * G
ysc = WY0 + (np.arange(ny) + 0.5) * G
for lay, sh in shapes.items():
    u = prep(unary_union(sh).intersection(box(WX0, WY0, WX1, WY1)))
    m = np.zeros((ny, nx), bool)
    for j, y in enumerate(ysc):
        row = [Point(x, y) for x in xsc]
        m[j] = [u.contains(pt) for pt in row]
    grid[lay] = m
L = {"F.Cu": 0, "B.Cu": 1}
VIAL = 1.6   # mm-equivalent cost of a layer change (board thickness)


def cell(x, y):
    return int((y - WY0) / G), int((x - WX0) / G)


via_cells = set(cell(*xy) for xy in links if WX0 < xy[0] < WX1 and WY0 < xy[1] < WY1)


def dijkstra(src, targets):
    dist = {src: 0.0}
    prev = {}
    pq = [(0.0, src)]
    steps = [(-1, 0, G), (1, 0, G), (0, -1, G), (0, 1, G), (-1, -1, G * 1.4142), (-1, 1, G * 1.4142),
             (1, -1, G * 1.4142), (1, 1, G * 1.4142)]
    while pq:
        d, s = heapq.heappop(pq)
        if d > dist.get(s, 1e9):
            continue
        if s in targets:
            path = [s]
            while path[-1] in prev:
                path.append(prev[path[-1]])
            return d, path[::-1]
        l, j, i = s
        m = grid["F.Cu" if l == 0 else "B.Cu"]
        nb = []
        for dj, di, c in steps:
            jj, ii = j + dj, i + di
            if 0 <= jj < ny and 0 <= ii < nx and m[jj, ii]:
                nb.append(((l, jj, ii), c))
        # a via: within 2 cells of a link point
        for vj, vi in via_cells:
            if abs(vj - j) <= 2 and abs(vi - i) <= 2:
                nb.append(((1 - l, j, i), VIAL))
                break
        for t, c in nb:
            nd = d + c
            if nd < dist.get(t, 1e9):
                dist[t] = nd
                prev[t] = s
                heapq.heappush(pq, (nd, t))
    return None, None


u8 = {p["num"]: p for p in fp["U8"]["pads"] if p["num"]}
tgt = set()
for num in ("22", "25", "28", "33"):
    for p in fp["U8"]["pads"]:
        if p["num"] == num:
            j, i = cell(p["x"], p["y"])
            for dj in (-1, 0, 1):
                for di in (-1, 0, 1):
                    tgt.add((0, j + dj, i + di))
                    if num == "33":
                        tgt.add((1, j + dj, i + di))
OUTPIN = {"C68": ("L1", "29"), "C69": ("L2", "27"), "C70": ("L3", "23"), "C71": ("L4", "21")}
bad = 0
for cref, (lref, pin) in OUTPIN.items():
    cg = [p for p in fp[cref]["pads"] if p["net"] == "GND"][0]
    cs = [p for p in fp[cref]["pads"] if p["net"] != "GND"][0]
    lp = {p["num"]: p for p in fp[lref]["pads"]}
    op = u8[pin]
    j, i = cell(cg["x"], cg["y"])
    d, path = dijkstra((0, j, i), tgt)
    if d is None:
        print(f"FAIL {cref}: no GND path to U8 in the window")
        bad += 1
        continue
    fwd = [(op["x"], op["y"]), (lp["1"]["x"], lp["1"]["y"]), (lp["2"]["x"], lp["2"]["y"]), (cs["x"], cs["y"]),
           (cg["x"], cg["y"])]
    flen = sum(math.dist(a, c) for a, c in zip(fwd, fwd[1:]))
    back = [(WX0 + (i + 0.5) * G, WY0 + (j + 0.5) * G) for _, j, i in path]
    poly = fwd + back[::-1][:0] + back[1:]
    area = Polygon(fwd + back[1:]).buffer(0).area if len(back) > 2 else 0.0
    vias = sum(1 for a, c in zip(path, path[1:]) if a[0] != c[0])
    ok = d <= LIM
    if "-v" in sys.argv:
        print("   path:", " ".join(f"{'FB'[a[0]]}({WX0 + a[2] * G:.0f},{WY0 + a[1] * G:.0f})" for a in path[::40]))
    bad += not ok
    print(f"{'PASS' if ok else 'FAIL'} {cref}: GND return {d:.1f} mm ({vias} layer change(s)) to U8; forward "
          f"{flen:.1f} mm; loop ~{area:.0f} mm2")
sys.exit(1 if bad else 0)
