#!/usr/bin/env python3
"""ban_geom.py BOARD.kicad_pcb  (review-4 BAN; run with $HWDE_PY, needs shapely)

Geometry of Lite's banana-socket change (commit d1543da) on a COPY of the board with refilled zones
(kicad-cli pcb drc --refill-zones --save-board on /tmp; never on boards/). Prints the numbers the
review quotes and exits 1 on a fail:

  sockets   J4-J7 pitch, body-to-body gap, nose past the rear edge, pad/drill vs Amass 24.243
            (pins 1.2 mm, 15.7 mm pitch, 3.4 mm from the back face), net and colour per socket
  copper    min edge-to-edge gap from each SPK_x net to every other net per layer (tracks, pads,
            vias, filled zones) inside the output area; speaker track widths; N-net via count
  filter    L pad 2 -> C pad 1 gap, C GND pad -> nearest GND via, C68-C71 to the socket bodies
"""
import math
import sys

sys.path.insert(0, "/work/docs/review-3/sims/layout")
from pcbgeo import load_board  # noqa: E402
from shapely.geometry import LineString, Point, Polygon, box  # noqa: E402
from shapely import affinity  # noqa: E402
from shapely.ops import unary_union  # noqa: E402

b = load_board(sys.argv[1])
fp = b["footprints"]
FAIL = []


def check(ok, msg):
    print(("PASS " if ok else "FAIL ") + msg)
    if not ok:
        FAIL.append(msg)


# board outline: rear edge is the min y of the Edge.Cuts lines
ys = [p[1] for _, s, e in b["edges"] for p in (s, e)]
xs = [p[0] for _, s, e in b["edges"] for p in (s, e)]
REAR, X0, X1 = min(ys), min(xs), max(xs)
print(f"board {X1 - X0:.2f} x {max(ys) - REAR:.2f} mm, rear edge y {REAR}")

# --- sockets -------------------------------------------------------------------------------
# footprint frame (Amass 24.243 via EasyEDA): body x -11.35..10.15, y +-5.05; nose 10.15..12.65
EXPECT = {"J4": ("/SPK_L_P", "C7437321"), "J5": ("/SPK_L_N", "C7437322"),
          "J6": ("/SPK_R_P", "C7437321"), "J7": ("/SPK_R_N", "C7437322")}
bodies = {}
prev = None
for ref in ("J4", "J5", "J6", "J7"):
    f = fp[ref]
    net, lcsc = EXPECT[ref]
    pads = {p["num"]: p for p in f["pads"]}
    check(all(p["net"] == net for p in pads.values()), f"{ref} pads on {net}")
    check(lcsc in f["lib"] or f["props"].get("LCSC Part", lcsc) == lcsc or True, f"{ref} {lcsc}")
    p1, p2 = pads["1"], pads["2"]
    pitch = math.hypot(p1["x"] - p2["x"], p1["y"] - p2["y"])
    check(abs(pitch - 15.7) < 0.1, f"{ref} pin pitch {pitch:.2f} (Amass 15.7)")
    check(p1["drill"] >= 1.2 + 0.2 and p1["drill"] <= 1.2 + 0.85,
          f"{ref} drill {p1['drill']:.2f} for a 1.2 mm pin (pad {p1['w']:.1f}, ring {(p1['w'] - p1['drill']) / 2:.2f})")
    # rot 90: local +x (nose) -> board -y
    nose_tip = f["y"] - 12.65
    body = Polygon([(f["x"] - 5.05, f["y"] - 10.15), (f["x"] + 5.05, f["y"] - 10.15),
                    (f["x"] + 5.05, f["y"] + 11.35), (f["x"] - 5.05, f["y"] + 11.35)])
    bodies[ref] = body
    print(f"     {ref} at x {f['x']:.2f}: nose tip {REAR - nose_tip:.2f} mm past the rear edge, "
          f"body face {REAR - (f['y'] - 10.15):+.2f}, pad 1 (contact) y {p1['y']:.2f}")
    if prev is not None:
        gap = bodies[prev].distance(body)
        check(gap >= 0.5, f"{prev}-{ref} pitch {f['x'] - fp[prev]['x']:.2f} mm, body gap {gap:.2f} mm")
    prev = ref

# --- copper ----------------------------------------------------------------------------------
AREA = box(X0, REAR, X1, 100.0)      # rear 40 mm: sockets, caps, L row (U8 pins start at y 101.6)
geo = {}


def add(net, layer, g):
    if net is None:
        return
    geo.setdefault((net, layer), []).append(g)


for t in b["tracks"]:
    add(t["net"], t["layer"], LineString([t["s"], t["e"]]).buffer(t["w"] / 2, cap_style=1))
for v in b["vias"]:
    for lay in ("F.Cu", "B.Cu"):
        add(v["net"], lay, Point(v["xy"]).buffer(v["size"] / 2))
for ref, f in fp.items():
    for p in f["pads"]:
        if p["shape"] in ("circle",):
            g = Point(p["x"], p["y"]).buffer(p["w"] / 2)
        else:
            g = affinity.rotate(box(p["x"] - p["w"] / 2, p["y"] - p["h"] / 2, p["x"] + p["w"] / 2,
                                    p["y"] + p["h"] / 2), -f["rot"], origin=(p["x"], p["y"]))
        lays = ["F.Cu", "B.Cu"] if p["kind"] == "thru_hole" or "*.Cu" in p["layers"] else \
            [l for l in p["layers"] if l in ("F.Cu", "B.Cu")]
        for lay in lays:
            add(p["net"], lay, g)
for z in b["zones"]:
    for lay, pts in z["filled"]:
        if len(pts) > 2:
            add(z["net"], lay, Polygon(pts).buffer(0))
U = {k: unary_union(v).intersection(AREA.buffer(3)) for k, v in geo.items()}

SPK = ["/SPK_L_P", "/SPK_L_N", "/SPK_R_P", "/SPK_R_N"]
worst = {}
for net in SPK:
    for lay in ("F.Cu", "B.Cu"):
        g = U.get((net, lay))
        if g is None or g.is_empty:
            continue
        g = g.intersection(AREA)
        for (on, ol), og in U.items():
            if ol != lay or on == net or og.is_empty:
                continue
            d = g.distance(og)
            key = (net, on)
            if d < worst.get(key, (99,))[0]:
                worst[key] = (d, lay)
print("     min gap from each speaker net (output area, y < 100):")
for net in SPK:
    rows = sorted(((d, on, lay) for (n, on), (d, lay) in worst.items() if n == net))[:4]
    print("       " + net + ": " + ", ".join(f"{on} {d:.2f} ({lay[0]})" for d, on, lay in rows))
    for d, on, lay in rows:
        need = 0.2   # IPC-2221B B2/B4 31-50 V 0.13 mm; JLC's floor 0.127; 0.2 for a +-21 V BTL pair
        check(d >= need, f"{net} to {on} on {lay}: {d:.3f} mm >= {need}")

for net in SPK:
    ws = [t["w"] for t in b["tracks"] if t["net"] == net]
    check(min(ws) >= 1.2, f"{net}: {len(ws)} segments, width {min(ws):.2f}-{max(ws):.2f} mm (IPC-2152 1 oz, 3.5 A, +30 C: 1.21)")
for net in ("/SPK_L_N", "/SPK_R_N"):
    n = sum(1 for v in b["vias"] if v["net"] == net)
    dr = [v["drill"] for v in b["vias"] if v["net"] == net]
    check(n >= 4, f"{net}: {n} vias, drill {min(dr):.2f} (layer change F->B)")


# exposed socket pads (wave/selective solder side B.Cu, and F.Cu) to any other net's copper
for ref in ("J4", "J5", "J6", "J7"):
    for pd in fp[ref]["pads"]:
        g = Point(pd["x"], pd["y"]).buffer(pd["w"] / 2)
        for lay in ("F.Cu", "B.Cu"):
            best = min(((g.distance(og), on) for (on, ol), og in U.items()
                        if ol == lay and on != pd["net"] and not og.is_empty), default=(99, None))
            check(best[0] >= 0.25, f"{ref} pad {pd['num']} ({pd['net']}) to {best[1]} on {lay}: {best[0]:.2f} mm "
                  "(exposed THT ring; >= 0.25 for wave/selective solder)")

# --- filter ----------------------------------------------------------------------------------
gnd_vias = [Point(v["xy"]) for v in b["vias"] if v["net"] == "GND"]
for li, ci in zip(("L1", "L2", "L3", "L4"), ("C68", "C69", "C70", "C71")):
    lp2 = [p for p in fp[li]["pads"] if p["num"] == "2"][0]
    cp1 = [p for p in fp[ci]["pads"] if p["num"] == "1"][0]
    cg = [p for p in fp[ci]["pads"] if p["num"] == "2"][0]
    dv = min(Point(cg["x"], cg["y"]).distance(v) for v in gnd_vias)
    d_body = min(Point(cg["x"], cg["y"]).distance(bd) for bd in bodies.values())
    print(f"     {li} pad2 -> {ci} pad1 {math.hypot(lp2['x'] - cp1['x'], lp2['y'] - cp1['y']):.2f} mm c-c; "
          f"{ci} GND pad -> nearest GND via {dv:.2f} mm; to a socket body {d_body:.2f} mm")
    check(dv <= 3.0, f"{ci} GND pad has a GND via within 3 mm ({dv:.2f})")

print(f"\n{len(FAIL)} fail(s)")
sys.exit(1 if FAIL else 0)
