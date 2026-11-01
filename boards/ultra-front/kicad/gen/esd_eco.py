"""ESD clamp stubs at the switches (review round 4, R5-UF-2): run once after
`hwde board_update --placements kicad/esd-placements.json`.
    KiCad's python: kicad/gen/esd_eco.py kicad/ultra-front.kicad_pcb

D2 (PESD5V0S2BT) sits in the gap between SW1 and SW2, D3 in the gap between
SW2 and SW3, both on F.Cu. BTN2 and BTN3 drop straight from the pad of the
switch below; BTN1 comes down from SW1's top through two vias (SW1's own GND
pad row and ENC_A/ENC_B cross under it on F.Cu); ENC_SW runs on F.Cu from
SW4's pin D, under the knob's pin row. Each clamp's GND pin has its own via.
docs/boards/ultra-front-esd.md.
"""
import sys

import pcbnew

b = pcbnew.LoadBoard(sys.argv[1])
MM = pcbnew.FromMM

TRACKS = [  # net, width, layer, points
    ("/BTN2", 0.2, "F.Cu", [(75.15, 41.75), (75.15, 43.05), (77.055, 44.95)]),
    ("/BTN1", 0.2, "F.Cu", [(75.15, 39.85), (75.75, 39.85), (76.6, 39.0)]),
    ("/BTN1", 0.2, "B.Cu", [(76.6, 39.0), (77.8, 37.8), (77.8, 33.627)]),
    ("GND", 0.4, "F.Cu", [(72.85, 40.8), (71.6, 40.8)]),
    ("/BTN3", 0.2, "F.Cu", [(75.15, 54.55), (75.15, 55.05), (77.055, 56.95)]),
    ("/ENC_SW", 0.2, "F.Cu", [(91.595, 55.05), (90.195, 56.45), (80.6, 56.45),
                              (76.8, 52.65), (75.15, 52.65)]),
    ("GND", 0.4, "F.Cu", [(72.85, 53.6), (71.6, 53.6)]),
]
VIAS = [("/BTN1", 76.6, 39.0), ("/BTN1", 77.8, 33.627),
        ("GND", 71.6, 40.8), ("GND", 71.6, 53.6)]

# BTN1's run from SW1 pin 2 to R4 is split where the via drops, so the stub
# joins at segment ends (check_esd follows ends, not T's).
SPLIT = ("/BTN1", (77.055, 33.627), (78.398, 33.627), (77.8, 33.627))
near = lambda p, x, y: abs(p.x - MM(x)) < 2000 and abs(p.y - MM(y)) < 2000
for t in list(b.GetTracks()):
    if t.GetClass() == "PCB_TRACK" and t.GetNetname() == SPLIT[0] \
            and near(t.GetStart(), *SPLIT[1]) and near(t.GetEnd(), *SPLIT[2]):
        n = pcbnew.PCB_TRACK(b)
        n.SetStart(pcbnew.VECTOR2I(MM(SPLIT[3][0]), MM(SPLIT[3][1])))
        n.SetEnd(t.GetEnd())
        n.SetWidth(t.GetWidth())
        n.SetLayer(t.GetLayer())
        n.SetNet(t.GetNet())
        t.SetEnd(pcbnew.VECTOR2I(MM(SPLIT[3][0]), MM(SPLIT[3][1])))
        b.Add(n)
        break
else:
    sys.exit("BTN1 segment to split not found")

for net, w, layer, pts in TRACKS:
    for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(b)
        t.SetStart(pcbnew.VECTOR2I(MM(x1), MM(y1)))
        t.SetEnd(pcbnew.VECTOR2I(MM(x2), MM(y2)))
        t.SetWidth(MM(w))
        t.SetLayer(b.GetLayerID(layer))
        t.SetNet(b.FindNet(net))
        b.Add(t)
for net, x, y in VIAS:
    v = pcbnew.PCB_VIA(b)
    v.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
    v.SetWidth(MM(0.6))
    v.SetDrill(MM(0.3))
    v.SetNet(b.FindNet(net))
    b.Add(v)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(sys.argv[1])
print(len(TRACKS), "tracks,", len(VIAS), "vias")
