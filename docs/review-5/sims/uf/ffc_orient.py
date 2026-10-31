"""review-5 UF: core J3 <-> ultra-front J1 over the FFC, pin for pin, and the cable type.
Reads both .kicad_pcb files and enclosure/core_box.py's FB_X (front board X 0 at core x FB_X).
Checks:
 1. J3 pin N and J1 pin N carry the same signal (net names compared after the
    known renames: FFC_SCK=LCD_SCK, FFC_MOSI=LCD_MOSI, LCD_BL=LCD_BL_PWM,
    EXP_INT=FRONT_INT_N, IR_IN=IR_OUT), and both are the same part at rot 0 on F.Cu.
 2. Each connector's mouth (the hold-down tab side, +y in the footprint: XUNPU
    FPC-05F-20PH20 drawing, terminals on the far side from the actuator and tabs)
    faces the board edge the cable comes from: core J3 its front (max y) edge,
    J1 the front board's bottom (max y, the edge nearest the core) edge.
 3. Conductor order: the path J3 -> down -> forward under the front board -> up
    into J1 bends only about the box x axis, so a conductor keeps its box x.
    Box x of J3 pin 1 must equal box x of J1 pin 1 (front board seen from the
    front: its KiCad +x is box +x, core_box.fb_point), within 1 mm.
 4. Contact face: bottom-contact at both ends; the three bends turn the J3-end
    contact face (down) to face forward at J1, whose contacts face rearward
    (into the board): so the far end needs its contacts on the other face = type B.
Exit 1 on a failure."""
import math, os, re, sys
H = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(H, '../../../review-3/sims/amp'))
import pcbq
R = os.path.join(H, '../../../..')
core = pcbq.footprints(pcbq.load(f"{R}/boards/core/kicad/core.kicad_pcb"))
fr_b = pcbq.load(f"{R}/boards/ultra-front/kicad/ultra-front.kicad_pcb")
front = pcbq.footprints(fr_b)
def edges(path):
    xs, ys = [], []
    t = open(path).read()
    for m in re.finditer(r'\(gr_(?:line|arc|rect)\s+\(start ([-\d.]+) ([-\d.]+)\)(?:\s+\(mid [^)]*\))?\s+\(end ([-\d.]+) ([-\d.]+)\).*?\(layer "Edge.Cuts"\)', t, re.S):
        xs += [float(m.group(1)), float(m.group(3))]; ys += [float(m.group(2)), float(m.group(4))]
    return min(xs), max(xs), min(ys), max(ys)
ce, fe = edges(f"{R}/boards/core/kicad/core.kicad_pcb"), edges(f"{R}/boards/ultra-front/kicad/ultra-front.kicad_pcb")
FB_X = float(re.search(r'^FB_X = ([\d.]+)', open(f"{R}/enclosure/core_box.py").read(), re.M).group(1))
ok = True
def say(c, m):
    global ok
    ok &= c; print(('PASS ' if c else 'FAIL ') + m)
J3, J1 = core['J3'], front['J1']
say(J3['lib'] == J1['lib'] and J3['rot'] == J1['rot'] == 0 and J3['layer'] == J1['layer'] == 'F.Cu',
    f"same part both ends ({J3['lib']}), rot {J3['rot']}/{J1['rot']}, {J3['layer']}/{J1['layer']}")
ren = {'FFC_SCK': 'LCD_SCK', 'FFC_MOSI': 'LCD_MOSI', 'LCD_BL': 'LCD_BL_PWM', 'EXP_INT': 'FRONT_INT_N', 'IR_IN': 'IR_OUT'}
n3 = {p['num']: (p['net'] or '').split('/')[-1] for p in J3['pads']}
n1 = {p['num']: (p['net'] or '').split('/')[-1] for p in J1['pads']}
mism = [(k, n3[k], n1.get(k)) for k in n3 if ren.get(n3[k], n3[k]) != n1.get(k)]
say(not mism, f"pin N = pin N for 22 pads {mism or ''}")
rev = [(k, n3[k], n1[str(21 - int(k))]) for k in n3 if int(k) <= 20]
rails = [r for r in rev if r[1] in ('+3V3', '+5V', 'GND') or r[2] in ('+3V3', '+5V', 'GND')]
fights = [r for r in rails if ren.get(r[1], r[1]) != r[2]]
print(f"  reversed cable (pin N -> 21-N): rail pins land on {[(a, b, c) for a, b, c in fights] or 'their own rails'}")
for name, f, e in (('J3 (core)', J3, ce), ('J1 (front)', J1, fe)):
    sig = [p for p in f['pads'] if p['num'] == '1'][0]; tab = [p for p in f['pads'] if p['num'] == '21'][0]
    to_edge = e[3] - f['y']
    say(tab['y'] > sig['y'] and to_edge < 6, f"{name}: mouth (tab side) toward max-y edge, centre {to_edge:.2f} mm from it")
x3 = [p for p in J3['pads'] if p['num'] == '1'][0]['x'] - ce[0]
x1 = FB_X + [p for p in J1['pads'] if p['num'] == '1'][0]['x'] - fe[0]
say(abs(x3 - x1) < 1.0, f"pin 1 box x: core J3 {x3:.2f}, front J1 {x1:.2f} (FB_X {FB_X}); conductor order kept by x-axis bends")
def rotx(v, deg):
    c, s = round(math.cos(math.radians(deg))), round(math.sin(math.radians(deg)))
    x, y, z = v; return (x, y * c - z * s, y * s + z * c)
t, n = (0, -1, 0), (0, 0, -1)   # box frame: x right, y back, z up; cable runs forward (-y), contacts down at J3
for name, a in (("down", +90), ("forward", -90), ("up", -90)):
    t, n = rotx(t, a), rotx(n, a)
say(t == (0, 0, 1), f"cable enters J1 running up {t}")
need = (0, 1, 0)                 # J1 on the front board's front face, contacts face rearward (+y, into the board)
print(f"  at J1 the J3-end contact face points {n}; J1 needs {need}")
say(n == tuple(-v for v in need), "far-end contacts must be on the opposite face: type B (opposite-side) cable, as enclosure/README.md says")
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
