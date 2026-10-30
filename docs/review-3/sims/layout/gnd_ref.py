#!/usr/bin/env python3
"""gnd_ref.py - is there GND copper under each sensitive trace? (review-3 LAYOUT)

For every track segment of the named nets, samples points every 0.25 mm and
checks that the adjacent copper layer (2-layer: the other side; 4-layer: the
nearest inner plane, In1.Cu for F.Cu and In2.Cu for B.Cu) has a filled GND
zone there. Reports per net: length, % referenced, longest unreferenced run
(the return current's detour starts there).

Usage: gnd_ref.py BOARD.kicad_pcb NET[,NET...] [--min-pct 90] [--max-gap 3]
Board should be a refilled COPY (kicad-cli drc --refill-zones --save-board).
Exit 1 if any net is below --min-pct or has a gap longer than --max-gap mm.
Also prints, for the whole board, the % of the board-outline bbox covered by
GND on each copper layer (a coarse plane-integrity number).
"""
import math, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pcbgeo import load_board, point_in_poly


class Grid:
    """bbox-indexed GND fills per layer for fast point lookups"""
    def __init__(self, b):
        self.polys = {}
        for z in b["zones"]:
            if z["net"] != "GND":
                continue
            for lay, pts in z["filled"]:
                xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
                self.polys.setdefault(lay, []).append((min(xs), min(ys), max(xs), max(ys), pts))
        self.any = {}
        for z in b["zones"]:
            if z["net"] in (None, "GND"):
                continue
            for lay, pts in z["filled"]:
                xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
                self.any.setdefault(lay, []).append((min(xs), min(ys), max(xs), max(ys), pts, z["net"]))

    def plane_net(self, x, y, lay):
        if self.gnd(x, y, lay):
            return "GND"
        for x0, y0, x1, y1, pts, net in self.any.get(lay, []):
            if x0 <= x <= x1 and y0 <= y <= y1 and point_in_poly(x, y, pts):
                return net
        return None

    def gnd(self, x, y, lay):
        for x0, y0, x1, y1, pts in self.polys.get(lay, []):
            if x0 <= x <= x1 and y0 <= y <= y1 and point_in_poly(x, y, pts):
                return True
        return False


def ref_layer(layers, lay):
    cu = [l for l in layers if l.endswith(".Cu")]
    if len(cu) <= 2:
        return "B.Cu" if lay == "F.Cu" else "F.Cu"
    # 4-layer JLC stack: F/In1 and In2/B are 0.24 mm apart, In1/In2 1.07 mm
    return {"F.Cu": "In1.Cu", "B.Cu": "In2.Cu", "In1.Cu": "F.Cu", "In2.Cu": "B.Cu"}.get(lay, None)


def main():
    a = sys.argv[1:]
    pcb, nets = a[0], a[1].split(",")
    minp = float(a[a.index("--min-pct") + 1]) if "--min-pct" in a else 90
    maxg = float(a[a.index("--max-gap") + 1]) if "--max-gap" in a else 3
    b = load_board(pcb)
    g = Grid(b)
    bad = []
    for net in nets:
        L = ok = 0.0
        gap = run = 0.0
        where = None
        under = {}
        splits = 0
        for t in b["tracks"]:
            if t["net"] != net:
                continue
            rl = ref_layer(b["layers"], t["layer"])
            (x1, y1), (x2, y2) = t["s"], t["e"]
            n = max(1, int(math.hypot(x2 - x1, y2 - y1) / 0.25))
            prev = None
            for i in range(n):
                x = x1 + (x2 - x1) * (i + 0.5) / n; y = y1 + (y2 - y1) * (i + 0.5) / n
                step = math.hypot(x2 - x1, y2 - y1) / n
                L += step
                pn = g.plane_net(x, y, rl) if rl else None
                under[pn] = under.get(pn, 0) + step
                if i > 0 and pn != prev:
                    splits += 1
                prev = pn
                if pn == "GND":
                    ok += step; run = 0
                else:
                    run += step
                    if run > gap:
                        gap, where = run, (round(x, 1), round(y, 1), t["layer"])
        if L == 0:
            print(f"{net:24s} no tracks"); continue
        pct = 100 * ok / L
        flag = "FAIL" if (pct < minp or gap > maxg) else "ok"
        if flag == "FAIL":
            bad.append(net)
        us = ", ".join(f"{k}:{v:.0f}" for k, v in sorted(under.items(), key=lambda kv: -kv[1]))
        print(f"{net:24s} len {L:6.1f} mm  over GND {pct:5.1f} %  longest gap {gap:4.1f} mm at {where}  {flag}   [ref under: {us}; in-segment plane changes {splits}]")
    # coverage
    xs = [p[0] for e in b["edges"] for p in e[1:]]; ys = [p[1] for e in b["edges"] for p in e[1:]]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    for lay in [l for l in b["layers"] if l.endswith(".Cu")]:
        n = c = 0
        y = y0 + 0.5
        while y < y1:
            x = x0 + 0.5
            while x < x1:
                n += 1; c += g.gnd(x, y, lay); x += 1.0
            y += 1.0
        print(f"GND coverage {lay}: {100*c/n:.0f} % of the {x1-x0:.0f} x {y1-y0:.0f} mm bbox")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
