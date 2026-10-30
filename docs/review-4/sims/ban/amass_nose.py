#!/usr/bin/env python3
"""amass_nose.py DRAWING.svg  (review-4 BAN; run with python3 -I)

Measures the Amass 24.243 drawing (LCSC datasheet C7437321, "Product Specification VER 2.0", one
page) from its vector paths and checks Lite's enclosure constants against it.

  get the drawing:  mkdir -p /tmp/ds3/ban/amass243 && cd /tmp/ds3/ban/amass243 &&
    curl -sL -A Mozilla/5.0 -o ds3.pdf \
      'https://datasheet.lcsc.com/datasheet/pdf/5f5cbca1951429c0db192cc2473b8452.pdf?productCode=C7437321'
    pdftocairo -svg ds3.pdf d.svg

Scale: the overall-length dimension (24 mm) = body back face to nose face. Reads: body height, nose
(the round collar round the plug bore: LCSC's photos of C7437321/C7437322 show it round) height and
its bottom over the board, bore axis (the red centre line) over the board, pin positions.
Checks against enclosure/core_box.py BANANA_NOSE / BANANA_AXIS / BANANA_HOLE: the nose must pass the
hole. Exits 1 if it does not.
"""
import re
import sys

svg = open(sys.argv[1], encoding="utf-8").read()
segs = []
for a in re.findall(r"<path([^>]*)/>", svg):
    if "stroke" not in a:
        continue
    d = re.search(r' d="([^"]*)"', a).group(1)
    tr = re.search(r'transform="matrix\(([^)]*)\)"', a)
    col = re.search(r'stroke="([^"]*)"', a)
    col = col.group(1) if col else ""
    m = [float(x) for x in tr.group(1).split(",")] if tr else [1, 0, 0, 1, 0, 0]
    pts = [(m[0] * float(x) + m[2] * float(y) + m[4], m[1] * float(x) + m[3] * float(y) + m[5])
           for _, x, y in re.findall(r"([ML]) ([-\d.]+) ([-\d.]+)", d)]
    segs += [(col, p, q) for p, q in zip(pts, pts[1:])]

BLACK, RED = "rgb(0%, 0%, 0%)", "rgb(100%, 0%, 0%)"
red = [(p, q) for c, p, q in segs if c == RED][0]            # the bore centre line: the drawing's frame
rx0, rx1 = min(red[0][0], red[1][0]) - 10, max(red[0][0], red[1][0]) + 10
ry = red[0][1]
blk = [(p, q) for c, p, q in segs if c == BLACK and all(rx0 <= t[0] <= rx1 and ry - 60 <= t[1] <= ry + 110 for t in (p, q))]
horiz = [(min(p[0], q[0]), max(p[0], q[0]), p[1]) for p, q in blk if abs(p[1] - q[1]) < 0.05 and abs(p[0] - q[0]) > 1]
vert = [(p[0], min(p[1], q[1]), max(p[1], q[1])) for p, q in blk if abs(p[0] - q[0]) < 0.05 and abs(p[1] - q[1]) > 1]
x_back = min(v[0] for v in vert if v[2] - v[1] > 60)            # back face (full-height vertical)
at_back = [h[2] for h in horiz if abs(h[0] - x_back) < 0.5]     # body edges start at the back face
top, bot = min(at_back), max(at_back)
x_nose = max(v[0] for v in vert)                                # nose face
nose_v = [v for v in vert if abs(v[0] - x_nose) < 0.05][0]
s = (x_nose - x_back) / 24.0                                    # pt per mm
axis_y = (red[0][1] + red[1][1]) / 2
pins = sorted(v for v in vert if v[1] >= bot - 0.5 and v[2] > bot + 20)
body_h = (bot - top) / s
nose_h = (nose_v[2] - nose_v[1]) / s
nose_bot = (bot - nose_v[2]) / s
axis = (bot - axis_y) / s
print(f"scale {s:.3f} pt/mm; body height {body_h:.2f} mm (drawing: 19 - 6 = 13)")
print(f"nose (collar) {nose_h:.2f} mm tall, {(x_nose - max(v[0] for v in vert if v[0] < x_nose - 1)) / s:.2f} mm long, "
      f"bottom {nose_bot:.2f} mm over the board, bore axis {axis:.2f} mm over the board")
if len(pins) >= 4:
    c1 = (pins[0][0] + pins[1][0]) / 2
    c2 = (pins[-2][0] + pins[-1][0]) / 2
    print(f"pins {(pins[1][0] - pins[0][0]) / s:.2f} mm wide; pin 1 {(c1 - x_back) / s:.2f} mm from the back face, "
          f"pin 2 {(x_nose - c2) / s:.2f} mm from the nose face, pitch {(c2 - c1) / s:.2f}")

src = open("/work/enclosure/core_box.py", encoding="utf-8").read()
const = {k: float(re.search(rf"^{k} = ([\d.]+)", src, re.M).group(1)) for k in ("BANANA_NOSE", "BANANA_AXIS", "BANANA_HOLE")}
print("enclosure:", const)
lo = const["BANANA_AXIS"] - const["BANANA_HOLE"] / 2
hi = const["BANANA_AXIS"] + const["BANANA_HOLE"] / 2
n_lo, n_hi = axis - nose_h / 2, axis + nose_h / 2
fails = []
if nose_h > const["BANANA_HOLE"]:
    fails.append(f"nose {nose_h:.2f} mm > hole {const['BANANA_HOLE']} mm")
if n_lo < lo or n_hi > hi:
    fails.append(f"nose spans {n_lo:.2f}-{n_hi:.2f} mm over the board, hole {lo:.2f}-{hi:.2f}")
if abs(axis - const["BANANA_AXIS"]) > 0.3:
    fails.append(f"bore axis {axis:.2f} mm vs BANANA_AXIS {const['BANANA_AXIS']}")
for f in fails:
    print("FAIL", f)
print("PASS nose passes the hole" if not fails else f"{len(fails)} fail(s)")
sys.exit(1 if fails else 0)
