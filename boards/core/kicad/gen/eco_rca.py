"""RCA jacks J4/J5 (XDJK-0271-1650) get the library's holes (review-3 R3-LAYOUT-12).

Round 1 (DFM-1/DFM-2) fixed the library footprint (lib/aiee.pretty: pins 1/3
as plated 1.6 x 2.4 mm slots at y -1.55, the snap-in legs' 2.0 x 3.0 mm NPTH
at (+-7.65, +1.60)) but the instances on core's routed board kept the old
pattern: three round 1.6 mm holes and no leg holes, so the jack can't be
inserted. This puts the library's pads on J4/J5 in place; tracks still end
inside the grown pads 1/3, and pad 2 doesn't move.

    KiCad's python: kicad/gen/eco_rca.py kicad/core.kicad_pcb
"""
import sys

import pcbnew

pcb = sys.argv[1]
M = pcbnew.FromMM
SLOT_Y = -1.55
SLOT = (1.6, 2.4)
SLOT_PAD = (2.5, 3.3)
LEG = (2.0, 3.0)
LEGS = ((-7.65, 1.60), (7.65, 1.60))

b = pcbnew.LoadBoard(pcb)
for ref in ("J4", "J5"):
    fp = b.FindFootprintByReference(ref)
    for p in list(fp.Pads()):
        if p.GetNumber() in ("1", "3"):
            off = p.GetFPRelativePosition()
            p.SetFPRelativePosition(pcbnew.VECTOR2I(off.x, M(SLOT_Y)))
            p.SetShape(pcbnew.PAD_SHAPE_OVAL)
            p.SetSize(pcbnew.VECTOR2I(M(SLOT_PAD[0]), M(SLOT_PAD[1])))
            p.SetDrillShape(pcbnew.PAD_DRILL_SHAPE_OBLONG)
            p.SetDrillSize(pcbnew.VECTOR2I(M(SLOT[0]), M(SLOT[1])))
        elif p.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
            fp.Remove(p)
    for x, y in LEGS:
        p = pcbnew.PAD(fp)
        p.SetAttribute(pcbnew.PAD_ATTRIB_NPTH)
        p.SetShape(pcbnew.PAD_SHAPE_OVAL)
        p.SetSize(pcbnew.VECTOR2I(M(LEG[0]), M(LEG[1])))
        p.SetDrillShape(pcbnew.PAD_DRILL_SHAPE_OBLONG)
        p.SetDrillSize(pcbnew.VECTOR2I(M(LEG[0]), M(LEG[1])))
        p.SetLayerSet(pcbnew.PAD.UnplatedHoleMask())
        p.SetNumber("")
        fp.Add(p)
        p.SetFPRelativePosition(pcbnew.VECTOR2I(M(x), M(y)))
    print(ref, "holes from the library")
b.Save(pcb)
