#!/usr/bin/env python3
"""Read what an enclosure needs from a .kicad_pcb, with no KiCad installed.

    python3 enclosure/kicad_board.py BOARD.kicad_pcb > enclosure/<board>.board.json

It writes board coordinates (u right, v down, mm) from the Edge.Cuts outline's
top-left corner, the same frame hwde's place scripts use: the outline size and
corner radius, the mounting holes, and every footprint's reference, footprint
name, position, rotation, side and courtyard box (in board coordinates, after
rotation), and the box round its through-hole pads, where pins come out
underneath. Part heights are not in a .kicad_pcb; enclosure/heights.json gives
them by footprint name, and a footprint it doesn't list is reported.
"""
from __future__ import annotations

import json
import math
import re
import sys
from pathlib import Path

TOKEN = re.compile(r'\(|\)|"(?:\\.|[^"\\])*"|[^\s()"]+')


def parse(text: str):
    stack, cur = [], []
    for t in TOKEN.findall(text):
        if t == "(":
            stack.append(cur)
            cur = []
        elif t == ")":
            done, cur = cur, stack.pop()
            cur.append(done)
        else:
            cur.append(t[1:-1] if t.startswith('"') else t)
    return cur[0]


def kids(node, name):
    return [n for n in node if isinstance(n, list) and n and n[0] == name]


def kid(node, name):
    k = kids(node, name)
    return k[0] if k else None


def xy(n):
    return float(n[1]), float(n[2])


def rot(p, deg):
    # KiCad's rotation is counter-clockwise on screen with y down.
    a = math.radians(-deg)
    return (p[0] * math.cos(a) - p[1] * math.sin(a), p[0] * math.sin(a) + p[1] * math.cos(a))


def shape_points(g):
    pts = []
    for key in ("start", "end", "mid", "center"):
        k = kid(g, key)
        if k:
            pts.append(xy(k))
    pl = kid(g, "pts")
    if pl:
        pts += [xy(p) for p in kids(pl, "xy")]
    if g[0] == "fp_circle" and kid(g, "center") and kid(g, "end"):
        c, e = xy(kid(g, "center")), xy(kid(g, "end"))
        r = math.dist(c, e)
        pts += [(c[0] - r, c[1] - r), (c[0] + r, c[1] + r)]
    return pts


def layer_of(g):
    k = kid(g, "layer")
    return k[1] if k else None


def board(path: Path) -> dict:
    root = parse(path.read_text())
    edge = []
    arcs = 0
    for g in root:
        if isinstance(g, list) and g and g[0].startswith("gr_") and layer_of(g) == "Edge.Cuts":
            edge += shape_points(g)
            arcs += g[0] == "gr_arc"
    xs, ys = [p[0] for p in edge], [p[1] for p in edge]
    x0, y0, x1, y1 = min(xs), min(ys), max(xs), max(ys)
    radius = 0.0
    for g in root:
        if isinstance(g, list) and g and g[0] == "gr_arc" and layer_of(g) == "Edge.Cuts":
            s, m = xy(kid(g, "start")), xy(kid(g, "mid"))
            # a 90-degree corner arc: start to mid is a chord of 45 degrees
            radius = round(math.dist(s, m) / (2 * math.sin(math.radians(22.5))), 3)
            break
    thick = float(kid(kid(root, "general"), "thickness")[1])
    fps, holes = [], []
    for f in kids(root, "footprint"):
        at = kid(f, "at")
        fx, fy = xy(at)
        fr = float(at[3]) if len(at) > 3 else 0.0
        side = "bottom" if kid(f, "layer")[1].startswith("B.") else "top"
        ref = None
        for p in kids(f, "property"):
            if p[1] == "Reference":
                ref = p[2]
        for t in kids(f, "fp_text"):
            if t[1] == "reference":
                ref = t[2]
        local = []
        for g in f:
            if isinstance(g, list) and g and g[0].startswith("fp_") and layer_of(g) in ("F.CrtYd", "B.CrtYd"):
                local += shape_points(g)
        world = [rot(p, fr) for p in local]
        box = None
        if world:
            box = [round(fx + min(p[0] for p in world) - x0, 3), round(fy + min(p[1] for p in world) - y0, 3),
                   round(fx + max(p[0] for p in world) - x0, 3), round(fy + max(p[1] for p in world) - y0, 3)]
        # through-hole pads: where pins come out under the board
        tht = []
        for pad in kids(f, "pad"):
            if pad[2] == "thru_hole":
                px, py = xy(kid(pad, "at"))
                sx, sy = xy(kid(pad, "size"))
                r = max(sx, sy) / 2
                for cx, cy in ((px - r, py - r), (px + r, py + r), (px - r, py + r), (px + r, py - r)):
                    tht.append(rot((cx, cy), fr))
        pins = None
        if tht:
            pins = [round(fx + min(p[0] for p in tht) - x0, 3), round(fy + min(p[1] for p in tht) - y0, 3),
                    round(fx + max(p[0] for p in tht) - x0, 3), round(fy + max(p[1] for p in tht) - y0, 3)]
        name = f[1]
        fps.append({"ref": ref, "footprint": name, "u": round(fx - x0, 3), "v": round(fy - y0, 3),
                    "rot": fr, "side": side, "courtyard": box, "tht_pads": pins})
        if "MountingHole" in name:
            drill = None
            for pad in kids(f, "pad"):
                d = kid(pad, "drill")
                if d and (drill is None or float(d[1]) > drill):
                    drill = float(d[1])
            holes.append({"ref": ref, "u": round(fx - x0, 3), "v": round(fy - y0, 3), "drill": drill})
    return {"source": str(path.name), "outline": {"w": round(x1 - x0, 3), "h": round(y1 - y0, 3),
            "corner_radius": radius}, "thickness": thick,
            "holes": sorted(holes, key=lambda h: (h["v"], h["u"])), "footprints": fps}


if __name__ == "__main__":
    print(json.dumps(board(Path(sys.argv[1])), indent=1))
