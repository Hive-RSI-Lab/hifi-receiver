"""lite-main: ESD clamps at the USB-C, the line-in and the sub-out (issue #133).

board_update placed the five clamps on B.Cu (kicad/esd-placements.json,
reports/board-update-esd.json): the top side round J1, J2 and J3 is full.

- D3 (NUP2105L, CC2), U10 (USBLC6-2SC6, D+/D-) sit under J1's body, between
  its shell pins and the board edge. D4 (NUP2105L, CC1) sits right of J1.
- D5 (PESD5V0S2BT, line in) and D6 (PESD5V0S2BT, sub out) sit between their
  RCA jack's two signal pins; each line pad runs straight into its pin's oval
  pad and the common pad into the jack's sleeve (GND) pin, plus a GND via between
  the clamp's pads (the jack's F.Cu signal runs cross just above).

USB-C: before this, D+ left J1 on F.Cu over the top of B5 (CC2), so neither
CC2 nor the clamps could reach the pins. This redoes the 2-3 mm of the USB
pair under J1 and leaves the rest of each route as it was:
  - D+ drops to B.Cu at a via just above B6 and runs left at y 65.65 into the
    old D+ route (its via at 75.53/67.45 goes: the route is B.Cu on both sides);
    a second, F.Cu branch climbs to a via at U10 pin 6.
  - D- climbs from B7 on F.Cu to a via at 81.6/65.0, runs left at y 65.0 into
    the old D- route at x 74.83, and taps U10 pin 4 on the way.
  - CC2 climbs from B5 on F.Cu to a via by D3; CC1 from A5 to a via right of
    J1, then on B.Cu between J1's right shell pins to D4.
Each clamp's common pad gets a GND via (TI SLVA680 2.4); U10's also runs into
J1's shell pin 3. KiCad's python, after board_update:
  python3.11 kicad/gen/esd_eco.py kicad/lite-main.kicad_pcb
"""
import sys

import pcbnew

MM = 1e6
pcb = sys.argv[1]
b = pcbnew.LoadBoard(pcb)
F, B = pcbnew.F_Cu, pcbnew.B_Cu


def V(x, y):
    return pcbnew.VECTOR2I(int(round(x * MM)), int(round(y * MM)))


def near(p, xy, tol=0.002):
    return abs(p.x / MM - xy[0]) < tol and abs(p.y / MM - xy[1]) < tol


# ---- old copper to remove: (net, layer, start, end) and vias (net, x, y)
RIP = [
    # D+ over the top of B5 on F.Cu, down to its via at 75.53/67.45
    ("/USB_DP", F, (79.3071, 65.6614), (80.615, 66.9693)),
    ("/USB_DP", F, (76.9595, 65.6614), (79.3071, 65.6614)),
    ("/USB_DP", F, (76.9595, 65.6614), (76.2, 65.6614)),
    ("/USB_DP", F, (76.2, 65.6614), (75.5343, 66.3271)),
    ("/USB_DP", F, (75.5343, 66.3271), (75.5343, 67.4478)),
    ("/USB_DP", F, (75.5343, 67.4478), (75.5343, 67.0866)),
    # D- from B7 up to its via, and its B.Cu run left at y 65.68
    ("/USB_DM", F, (82.1857, 66.6198), (82.115, 66.6905)),
    ("/USB_DM", F, (82.115, 66.6905), (82.115, 67.773)),
    ("/USB_DM", B, (82.1857, 66.6198), (81.2467, 65.6808)),
    ("/USB_DM", B, (81.2467, 65.6808), (76.5537, 65.6808)),
    ("/USB_DM", B, (76.5537, 65.6808), (74.8288, 67.4057)),
]
RIP_VIAS = [("/USB_DP", 75.5343, 67.4478), ("/USB_DM", 82.1857, 66.6198)]

# ---- new copper: (net, layer, width, [points])
W, WG = 0.2, 0.3
PATHS = [
    # D+: B6 up to the via over the pin field, then B.Cu left into the old route
    ("/USB_DP", F, W, [(80.615, 66.9693), (80.615, 66.3)]),
    ("/USB_DP", B, W, [(80.615, 66.3), (79.965, 65.65), (75.9, 65.65),
                       (75.5343, 66.0157), (75.5343, 67.4478)]),
    # D+ branch to U10 pin 6 (F.Cu up past the D- via, then a via)
    ("/USB_DP", F, W, [(80.615, 66.3), (80.615, 65.885), (81.0, 65.5),
                       (81.0, 61.75), (80.9, 61.65)]),
    ("/USB_DP", B, W, [(80.9, 61.65), (81.75, 61.65)]),
    # D-: B7 up to its via; B.Cu to U10 pin 4 and left into the old route
    ("/USB_DM", F, W, [(82.115, 67.773), (82.115, 65.515), (81.6, 65.0)]),
    ("/USB_DM", B, W, [(81.6, 65.0), (81.75, 64.85), (81.75, 63.55)]),
    ("/USB_DM", B, W, [(81.6, 65.0), (76.0, 65.0), (74.8288, 66.1712),
                       (74.8288, 67.4057)]),
    # CC2: B5 up to a via, then D3 pin 1
    ("/CC2", F, W, [(79.615, 67.773), (79.615, 65.085), (80.25, 64.45)]),
    ("/CC2", B, W, [(80.25, 64.45), (80.25, 63.3)]),
    # CC1: A5 up and right to a via, B.Cu between J1's shell pins 3/4 to D4
    ("/CC1", F, W, [(82.615, 67.773), (82.615, 66.9), (83.0, 66.515), (83.0, 65.4)]),
    ("/CC1", B, W, [(83.0, 65.4), (83.4, 65.0), (87.4, 65.0)]),
    # ground: each common pad to its via; U10's also into J1's shell pin 3
    ("GND", B, WG, [(79.3, 61.3), (79.3, 62.4)]),
    ("GND", B, WG, [(84.05, 62.6), (85.3, 62.6)]),
    ("GND", B, WG, [(84.05, 62.6), (82.9, 62.6)]),
    ("GND", B, WG, [(89.4, 64.05), (90.5, 64.05)]),
    # line in: D5 pads straight into J2's oval pins; common pad into the sleeve
    ("/LINEIN_L", B, W, [(43.42, 77.75), (45.5, 77.75)]),
    ("/LINEIN_R", B, W, [(41.52, 77.75), (39.45, 77.75)]),
    ("GND", B, WG, [(42.47, 75.45), (42.47, 74.5)]),
    ("GND", B, WG, [(42.47, 75.45), (42.47, 76.6)]),
    # sub out: D6 likewise into J3
    ("/SUB_L", B, W, [(65.62, 77.75), (67.7, 77.75)]),
    ("/SUB_R", B, W, [(63.72, 77.75), (61.65, 77.75)]),
    ("GND", B, WG, [(64.67, 75.45), (64.67, 74.5)]),
    ("GND", B, WG, [(64.67, 75.45), (64.67, 76.6)]),
]
VIAS = [("/USB_DP", 80.615, 66.3), ("/USB_DP", 80.9, 61.65), ("/USB_DM", 81.6, 65.0),
        ("/CC2", 80.25, 64.45), ("/CC1", 83.0, 65.4),
        ("GND", 79.3, 62.4), ("GND", 82.9, 62.6), ("GND", 90.5, 64.05),
        ("GND", 42.47, 76.6), ("GND", 64.67, 76.6)]

gone = []
for t in b.GetTracks():
    if t.GetClass() == "PCB_VIA":
        if any(t.GetNetname() == net and near(t.GetPosition(), (x, y)) for net, x, y in RIP_VIAS):
            gone.append(t)
        continue
    if any(t.GetNetname() == net and t.GetLayer() == layer and (
            (near(t.GetStart(), a) and near(t.GetEnd(), z)) or
            (near(t.GetStart(), z) and near(t.GetEnd(), a))) for net, layer, a, z in RIP):
        gone.append(t)
ripped = len(gone)
for t in gone:
    b.Remove(t)
want = len(RIP) + len(RIP_VIAS)
if ripped != want:
    sys.exit(f"ripped {ripped} items, expected {want}: the board is not the one this ECO was written for")

for net, layer, w, pts in PATHS:
    for a, z in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(b)
        t.SetStart(V(*a)); t.SetEnd(V(*z)); t.SetWidth(int(w * MM))
        t.SetLayer(layer); t.SetNet(b.FindNet(net)); b.Add(t)
for net, x, y in VIAS:
    v = pcbnew.PCB_VIA(b)
    v.SetPosition(V(x, y)); v.SetDrill(int(0.3 * MM)); v.SetWidth(int(0.6 * MM))
    v.SetLayerPair(F, B); v.SetNet(b.FindNet(net)); b.Add(v)
for fp in b.GetFootprints():
    if fp.GetReference() in ("D3", "D4", "D5", "D6", "U10"):
        fp.Reference().SetVisible(False)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(pcb)
print(f"ripped {ripped}, added {sum(len(p[3]) - 1 for p in PATHS)} segments and {len(VIAS)} vias")
