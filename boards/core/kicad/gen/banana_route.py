"""core: speaker copper from the fan-in pours to the banana sockets J20-J27, stage 2 of the ECO (KiCad python).

banana_eco.py cut the eight speaker pours back to y 130. Each net now
climbs a column from its pour, runs along its own band between the sockets'
two pad rows (pin 1 at y 125.5, pin 2 at y 109.8), and drops onto its
socket's pin-2 spine. The pours keep their layers: L-/R+ on F.Cu, L+/R- on
B.Cu, four nets per layer. In pour order the nets reach their sockets in
the same left-to-right order on each layer, so bands stack (the further
right a net goes, the lower its band) and nothing crosses: no vias.
A spine runs on down to pin 1 where no later band on its layer crosses it.

Every piece is a solid-connected zone (priority 5, like power_zones.py's
pours), 2.0 mm or wider: 3.7 A rms continuous (constraints.json
power_notes) on 1 oz outer copper.

    python3.11 kicad/gen/banana_route.py kicad/core.kicad_pcb
"""
import sys

import pcbnew

MM = 1e6
P1, P2 = 125.4975, 109.7975          # socket pin rows
COL0 = 129.5                         # columns start inside the cut pours (y 130)
W = 1.0                              # half band/spine width
# net, layer, column (x0, x1), band centre y, socket x, spine to pin 1
NETS = [
    ("/amp_a/A_SPK_L-", "F.Cu", (45.3, 47.9), 112.8, 61.0, True),
    ("/A_SPK_R+", "F.Cu", (63.3, 65.9), 115.6, 72.0, True),
    ("/amp_b/B_SPK_L-", "F.Cu", (78.2, 80.8), 118.4, 105.0, False),
    ("/amp_b/B_SPK_R+", "F.Cu", (96.2, 98.8), 121.2, 116.0, True),
    ("/A_SPK_L+", "B.Cu", (42.6, 44.6), 112.8, 50.0, True),
    ("/amp_a/A_SPK_R-", "B.Cu", (54.3, 56.5), 115.6, 83.0, False),
    ("/amp_b/B_SPK_L+", "B.Cu", (75.3, 77.5), 118.4, 94.0, False),
    ("/amp_b/B_SPK_R-", "B.Cu", (87.2, 89.4), 121.2, 127.0, True),
]


def V(x, y):
    return pcbnew.VECTOR2I(int(round(x * MM)), int(round(y * MM)))


def rects(col, band, s, full):
    (c0, c1) = col
    return [(c0, band - W, c1, COL0),                       # column
            (c0, band - W, s + W, band + W),                # band
            (s - W, P2, s + W, P1 if full else band + W)]   # spine


def main(path):
    b = pcbnew.LoadBoard(path)
    proto = {}
    for z in b.Zones():
        if not z.GetIsRuleArea() and "SPK_" in z.GetNetname():
            proto.setdefault(b.GetLayerName(z.GetFirstLayer()), z)
    nets = b.GetNetsByName()
    keep = []
    for net, layer, col, band, s, full in NETS:
        for (x0, y0, x1, y1) in rects(col, band, s, full):
            z = proto[layer].Clone()
            z = pcbnew.Cast_to_BOARD_ITEM(z).Cast() if not hasattr(z, "Outline") else z
            z.SetNet(nets[net])
            o = z.Outline()
            o.RemoveAllContours()
            o.NewOutline()
            for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
                o.Append(V(x, y))
            z.SetZoneName("")
            b.Add(z)
            keep.append(z)
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    b.Save(path)
    print(len(keep), "zones")


if __name__ == "__main__":
    main(sys.argv[1])
