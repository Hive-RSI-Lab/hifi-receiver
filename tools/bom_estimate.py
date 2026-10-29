#!/usr/bin/env python3
"""Total docs/bom-estimate.csv per tier at 10 and 100 units, with a JLC
assembly estimate from hwde's jlc_pricing.yaml figures.

Usage: python3 tools/bom_estimate.py [docs/bom-estimate.csv]
       python3 tools/bom_estimate.py --unit lite 100   # one number, for a score

Parts are priced per board at the 10- and 100-board price break. Assembly is
the hwde table (setup 8, stencil 8, 3 per unique extended part, 0.0017 per SMT
joint) plus JLC's hand-soldered through-hole rate (0.0173 per joint) and
PCB price points for each tier's board(s). hwde measured its own PCB table as
1.9-3.1x low against JLC's real quotes, so the PCB line is reported both raw
and x2.5. Refresh prices with hwde's parts_search.py and re-run.
"""
import csv
import sys
from pathlib import Path

SETUP, STENCIL, FEEDER, SMT_JOINT, THT_JOINT = 8.0, 8.0, 3.0, 0.0017, 0.0173
PCB_CAL = 2.5
# board(s) per tier: (name, layers, w_mm, h_mm)
BOARDS = {
    "lite": [("lite-main", 2, 100, 90)],
    "plus": [("core", 4, 120, 100)],
    "ultra": [("core", 4, 120, 100), ("ultra-front", 2, 100, 40)],
}
PCB_BASE = {2: {10: 2.00, 100: 24.90}, 4: {10: 9.90, 100: 76.90}}


def pcb(layers, w, h, qty):
    # hwde's 100x100 price points scaled by area past 1 dm^2. hwde's own
    # oversize_per_dm2 term (11 $/dm^2 per 4-layer board) runs >10x above its
    # own 100x100 point (0.77 $/board at 100), so it is not used here.
    return PCB_BASE[layers][qty] * max(1.0, w * h / 10000)


def main(path, only=None):
    rows = list(csv.DictReader(open(path)))
    for tier in ("lite", "plus", "ultra"):
        if only and tier != only[0]:
            continue
        used = [r for r in rows if float(r[tier]) > 0]
        onboard = [r for r in used if r["type"] != "offboard"]
        ext = len({r["mpn"] for r in onboard if r["type"] == "extended"})
        smt = sum(float(r[tier]) * int(r["pins"]) for r in onboard if r["mount"] == "smt")
        tht = sum(float(r[tier]) * int(r["pins"]) for r in onboard if r["mount"] == "tht")
        if not only:
            print(f"== {tier}: {len(used)} lines, {ext} unique extended parts, "
                  f"{smt:.0f} SMT joints, {tht:.0f} THT joints per board")
        for q, col in ((10, "p10"), (100, "p100")):
            parts = sum(float(r[tier]) * float(r[col]) for r in onboard)
            off = sum(float(r[tier]) * float(r[col]) for r in used if r["type"] == "offboard")
            nb = len(BOARDS[tier])
            asm = nb * (SETUP + STENCIL) + ext * FEEDER + q * (smt * SMT_JOINT + tht * THT_JOINT)
            pcbs = sum(pcb(l, w, h, q) for _, l, w, h in BOARDS[tier])
            per = (parts * q + asm + pcbs * PCB_CAL) / q + off
            if only:
                if q == only[1]:
                    print(f"{per:.2f}")
                continue
            print(f"  x{q:<3} parts {parts:6.2f}/board  assembly {asm/q:6.2f}/board  "
                  f"pcb {pcbs/q:5.2f} raw ({pcbs*PCB_CAL/q:5.2f} x{PCB_CAL})/board  "
                  f"off-board {off:4.2f}  => {per:6.2f} per unit")


if __name__ == "__main__":
    default = Path(__file__).parent.parent / "docs/bom-estimate.csv"
    if len(sys.argv) == 4 and sys.argv[1] == "--unit":
        main(default, (sys.argv[2], int(sys.argv[3])))
    else:
        main(Path(sys.argv[1]) if len(sys.argv) > 1 else default)
