"""R5-PA: the speaker-current copper on core, measured. For each TPA3220 output
leg, the narrowest copper between the inductor's speaker-side pad and the
speaker terminal pad (and between the amp's OUT pins and the inductor), as the
largest width W for which the net's copper, eroded by W/2 on every layer
(zone fills + tracks + pads, joined across layers through the net's vias and
plated pads), still connects the two ends. Same idea as hwde check_current's
pour-neck rule, which here only ran at the nets' 0.5 A budget
(constraints.json), not at the speaker current.

Currents compared (4 ohm, 28 V): full-power sine at the clip 0.95*28/sqrt2/4 =
4.7 A rms; the 56 W 'governor cap' in constraints power_notes 3.74 A rms;
Plus's sustained thermal limit 9.6 W/ch 1.55 A rms. IPC-2152 widths from
hwde check_current's own 1 oz table (outer layers), dT 20 C.

Run with $HWDE_PY (shapely). Read-only. Exit 1 if any leg's neck is under
the IPC-2152 width for 3.74 A at dT 20 C on 1 oz."""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcbq
from shapely.geometry import Polygon, LineString, Point
from shapely.ops import unary_union

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../..'))
B = pcbq.load(f'{ROOT}/boards/core/kicad/core.kicad_pcb')
CU = ['F.Cu', 'In1.Cu', 'In2.Cu', 'B.Cu']
FP = pcbq.footprints(B)

def netname(node):
    n = pcbq.first(node, 'net')
    return n[1] if n and len(n) > 1 else None

def norm(n):
    return (n or '').lstrip('/').split('/')[-1]

def pad_poly(fp, p):
    r = math.radians(-fp['rot'])
    w, h = p['w'] / 2, p['h'] / 2
    pts = []
    for dx, dy in ((-w, -h), (w, -h), (w, h), (-w, h)):
        # pad-local rotation is folded into fp rotation in pcbq (pads at 0/90 only here)
        x = p['lx'] + dx; y = p['ly'] + dy
        pts.append((fp['x'] + x * math.cos(r) - y * math.sin(r), fp['y'] + x * math.sin(r) + y * math.cos(r)))
    return Polygon(pts)

def copper(net):
    """per layer: list of geometries; joints: list of (point, radius) through all layers"""
    lay = {l: [] for l in CU}
    joints = []
    for s in pcbq.find(B, 'segment') + pcbq.find(B, 'arc'):
        if norm(netname(s)) != net:
            continue
        st = pcbq.first(s, 'start'); en = pcbq.first(s, 'end')
        w = float(pcbq.first(s, 'width')[1]); ly = pcbq.first(s, 'layer')[1]
        pts = [(float(st[1]), float(st[2]))]
        mid = pcbq.first(s, 'mid')
        if mid:
            pts.append((float(mid[1]), float(mid[2])))
        pts.append((float(en[1]), float(en[2])))
        if ly in lay:
            lay[ly].append(LineString(pts).buffer(w / 2, cap_style=1))
    for v in pcbq.find(B, 'via'):
        if norm(netname(v)) != net:
            continue
        at = pcbq.first(v, 'at'); sz = float(pcbq.first(v, 'size')[1])
        c = Point(float(at[1]), float(at[2]))
        joints.append((c, sz / 2))
        for l in CU:
            lay[l].append(c.buffer(sz / 2))
    for z in pcbq.find(B, 'zone'):
        if norm(netname(z)) != net:
            continue
        for fpoly in pcbq.find(z, 'filled_polygon'):
            ly = pcbq.first(fpoly, 'layer')[1]
            pts = [(float(p[1]), float(p[2])) for p in pcbq.find(pcbq.first(fpoly, 'pts'), 'xy')]
            if ly in lay and len(pts) > 2:
                lay[ly].append(Polygon(pts).buffer(0))
    for ref, fp in FP.items():
        for p in fp['pads']:
            if norm(p['net']) != net:
                continue
            poly = pad_poly(fp, p)
            if p['kind'] == 'thru_hole':
                joints.append((poly.centroid, max(p['w'], p['h']) / 2))
                for l in CU:
                    lay[l].append(poly)
            else:
                lay[fp['layer'] if fp['layer'] in CU else 'F.Cu'].append(poly)
    return {l: unary_union(g) if g else None for l, g in lay.items()}, joints

def connected(net, cu, joints, ends, W):
    """ends: two lists of (layer, polygon). Eroded copper must link them."""
    pieces = []
    for l, g in cu.items():
        if g is None:
            continue
        e = g.buffer(-W / 2 + 1e-4)
        for gg in getattr(e, 'geoms', [e]):
            if not gg.is_empty:
                pieces.append((l, gg))
    n = len(pieces)
    parent = list(range(n))
    def f(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]; i = parent[i]
        return i
    for c, r in joints:
        hit = [i for i, (l, g) in enumerate(pieces) if g.distance(c) <= r]
        for i in hit[1:]:
            parent[f(i)] = f(hit[0])
    def touch(end):
        return {f(i) for i, (l, g) in enumerate(pieces) for (le, pe) in end if l == le and g.distance(pe) <= W / 2}
    return bool(touch(ends[0]) & touch(ends[1]))

def neck(net, ends):
    cu, joints = copper(net)
    lo, hi = 0.0, 6.0
    if not connected(net, cu, joints, ends, 0.05):
        return 0.0
    for _ in range(14):
        mid = (lo + hi) / 2
        if connected(net, cu, joints, ends, mid):
            lo = mid
        else:
            hi = mid
    return lo

def pads_of(ref, net):
    fp = FP[ref]
    out = []
    for p in fp['pads']:
        if norm(p['net']) == net:
            poly = pad_poly(fp, p)
            ls = CU if p['kind'] == 'thru_hole' else [fp['layer']]
            out += [(l, poly) for l in ls]
    return out

# IPC-2152 1 oz outer, dT 10 C, from hwde check_current (I -> width mm), scaled (10/dT)^0.44
IPC = [(0.0, 0.0), (0.5, 0.25), (1.0, 0.50), (2.0, 1.10), (3.0, 1.90), (4.0, 2.70), (5.0, 3.50), (6.0, 4.40)]
def width_for(i, dt):
    ie = i / ((dt / 10.0) ** 0.44)
    for (a, wa), (b, wb) in zip(IPC, IPC[1:]):
        if a <= ie <= b:
            return wa + (wb - wa) * (ie - a) / (b - a)
    return IPC[-1][1]

def amps_for(w, dt):
    lo, hi = 0.0, 8.0
    for _ in range(30):
        m = (lo + hi) / 2
        if width_for(m, dt) <= w:
            lo = m
        else:
            hi = m
    return lo

legs = []
for z, u, lbase, jbase in (("A", "U15", 1, 6), ("B", "U16", 5, 10)):
    outs = (("OUT1M", "SPK_L-", 0, 2), ("OUT1P", "SPK_L+", 0, 1), ("OUT2M", "SPK_R-", 1, 2), ("OUT2P", "SPK_R+", 1, 1))
    for k, (o, spk, jk, jp) in enumerate(outs):
        L = f"L{lbase + k}"; J = f"J{jbase + jk}"
        onet = f"{z}_{o}"; snet = f"{z}_{spk}"
        legs.append((f"{u} {onet} pins->{L}", onet, pads_of(u, onet), pads_of(L, onet)))
        legs.append((f"{L}->{J} {snet}", snet, pads_of(L, snet), pads_of(J, snet)))

need = width_for(3.74, 20)
bad = 0
print(f"IPC-2152 (1 oz outer) width: 3.74 A rms dT20 -> {need:.2f} mm; 1.55 A dT20 -> {width_for(1.55, 20):.2f} mm; 4.7 A dT20 -> {width_for(4.7, 20):.2f} mm")
for name, net, a, b in legs:
    w = neck(net, (a, b))
    flag = "ok  " if w >= need else ("LOW " if w >= width_for(1.55, 20) else "FAIL")
    if w < need:
        bad = 1
    print(f"{flag} {name:32s} neck {w:.2f} mm -> {amps_for(w, 10):.1f} A @dT10, {amps_for(w, 20):.1f} A @dT20, {amps_for(w, 40):.1f} A @dT40")
sys.exit(bad)
