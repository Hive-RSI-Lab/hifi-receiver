"""pcbgeo.py - read-only KiCad .kicad_pcb geometry helper (review-3 LAYOUT).

load_board(path) -> dict(footprints={ref: {...}}, tracks=[...], vias=[...],
zones=[...], outline=[segments], layers=[names]).
Pad coordinates are absolute board mm (y down). Zones carry their filled
polygons if the file has them (run kicad-cli drc --refill-zones --save-board
on a COPY in /tmp to get fresh fills).
"""
import math
import re


def _parse(text):
    tok = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text)
    stack = [[]]
    for t in tok:
        if t == '(':
            stack.append([])
        elif t == ')':
            x = stack.pop()
            stack[-1].append(x)
        else:
            stack[-1].append(t[1:-1] if t.startswith('"') else t)
    return stack[0][0]


def find(node, key):
    return [c for c in node if isinstance(c, list) and c and c[0] == key]


def first(node, key):
    r = find(node, key)
    return r[0] if r else None


def _xy(n):
    return float(n[1]), float(n[2])


def load_board(path):
    b = _parse(open(path, encoding="utf-8").read())
    netnames = {}
    for n in find(b, "net"):
        if len(n) >= 3:
            netnames[n[1]] = n[2]
    fps = {}
    for fp in find(b, "footprint"):
        props = {p[1]: p[2] for p in find(fp, "property") if len(p) > 2}
        ref = props.get("Reference")
        at = first(fp, "at")
        x, y = float(at[1]), float(at[2])
        rot = float(at[3]) if len(at) > 3 else 0.0
        layer = first(fp, "layer")[1]
        attr = first(fp, "attr")
        pads = []
        for pd in find(fp, "pad"):
            pat = first(pd, "at")
            sz = first(pd, "size")
            px, py = float(pat[1]), float(pat[2])
            r = math.radians(-rot)
            gx = x + px * math.cos(r) - py * math.sin(r)
            gy = y + px * math.sin(r) + py * math.cos(r)
            net = first(pd, "net")
            nn = None
            if net is not None:
                nn = net[-1] if len(net) > 2 else netnames.get(net[1], net[1])
            pf = first(pd, "pinfunction")
            dr = first(pd, "drill")
            lay = first(pd, "layers")
            pads.append(dict(num=pd[1], kind=pd[2], shape=pd[3], x=gx, y=gy, lx=px, ly=py,
                             w=float(sz[1]), h=float(sz[2]), net=nn,
                             pinfunction=pf[1] if pf else None,
                             drill=float(dr[1]) if dr and len(dr) > 1 and dr[1] != "oval" else (float(dr[2]) if dr and len(dr) > 2 else None),
                             layers=lay[1:] if lay else []))
        fps[ref] = dict(lib=fp[1], x=x, y=y, rot=rot, layer=layer, props=props, pads=pads,
                        attr=attr[1:] if attr else [])
    tracks = []
    for t in find(b, "segment"):
        s, e = _xy(first(t, "start")), _xy(first(t, "end"))
        net = first(t, "net")
        nn = net[-1] if net and len(net) > 2 else netnames.get(net[1], net[1]) if net else None
        tracks.append(dict(s=s, e=e, w=float(first(t, "width")[1]), layer=first(t, "layer")[1], net=nn))
    vias = []
    for v in find(b, "via"):
        net = first(v, "net")
        nn = net[-1] if net and len(net) > 2 else netnames.get(net[1], net[1]) if net else None
        vias.append(dict(xy=_xy(first(v, "at")), size=float(first(v, "size")[1]),
                         drill=float(first(v, "drill")[1]), net=nn))
    zones = []
    for z in find(b, "zone"):
        net = first(z, "net_name") or first(z, "net")
        layers = first(z, "layers") or first(z, "layer")
        polys = []
        for fpoly in find(z, "filled_polygon"):
            lay = first(fpoly, "layer")[1]
            pts = [(float(p[1]), float(p[2])) for p in first(fpoly, "pts")[1:] if p[0] == "xy"]
            polys.append((lay, pts))
        outline = [(float(p[1]), float(p[2])) for p in first(first(z, "polygon"), "pts")[1:] if p[0] == "xy"] if first(z, "polygon") else []
        zones.append(dict(net=net[1] if net else None, layers=layers[1:] if layers else [],
                          outline=outline, filled=polys,
                          keepout=first(z, "keepout") is not None))
    edges = []
    for g in find(b, "gr_line") + find(b, "gr_rect") + find(b, "gr_arc"):
        lay = first(g, "layer")
        if lay and lay[1] == "Edge.Cuts":
            edges.append((g[0], _xy(first(g, "start")), _xy(first(g, "end"))))
    layers = [l[1] for l in first(b, "layers")[1:] if isinstance(l, list)]
    return dict(footprints=fps, tracks=tracks, vias=vias, zones=zones, edges=edges, layers=layers)


def poly_area(pts):
    a = 0.0
    for i in range(len(pts)):
        x1, y1 = pts[i]
        x2, y2 = pts[(i + 1) % len(pts)]
        a += x1 * y2 - x2 * y1
    return abs(a) / 2


def point_in_poly(x, y, pts):
    c = False
    n = len(pts)
    j = n - 1
    for i in range(n):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if ((yi > y) != (yj > y)) and (x < (xj - xi) * (y - yi) / (yj - yi + 1e-12) + xi):
            c = not c
        j = i
    return c


def copper_at(board, x, y, layer):
    """nets of filled zones covering (x, y) on layer"""
    out = []
    for z in board["zones"]:
        for lay, pts in z["filled"]:
            if lay == layer and point_in_poly(x, y, pts):
                out.append(z["net"])
    return out
