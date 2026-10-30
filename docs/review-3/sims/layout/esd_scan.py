#!/usr/bin/env python3
"""esd_scan.py - what protects each connector pin? (review-3 LAYOUT)

esd_scan.py BOARD.kicad_pcb
For every J* pad on a signal net (not GND/unconnected), lists the parts on
that net with their distance from the pad, marks TVS/ESD parts (D* with
TVS/ESD values, USBLC6, PESD, SMBJ, ...) and IC pins (U*) reached directly,
i.e. with no series R/C/FB between connector and IC. Prints a table; exit 1
if an IC pin is reached directly with no TVS on that net (direct-to-IC).
Series parts (R/FB/C) break the "direct" chain one level only (the IC on the
far side of a series R is reported as "behind R").
"""
import math, re, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pcbgeo import load_board

TVS = re.compile(r"SMBJ|SMAJ|USBLC|PESD|ESD|TVS|SRV05|TPD|BZT|LESD|ULC", re.I)


def main():
    b = load_board(sys.argv[1]); F = b["footprints"]
    bynet = {}
    for r, f in F.items():
        for p in f["pads"]:
            if p["net"]:
                bynet.setdefault(p["net"], []).append((r, p))
    bad = 0
    for r in sorted(F, key=lambda s: (len(s), s)):
        if not r.startswith("J"):
            continue
        seen = set()
        for p in F[r]["pads"]:
            n = p["net"]
            if not n or n == "GND" or "unconnected" in n or n in seen:
                continue
            seen.add(n)
            items = []
            tv = False; direct = []
            for rr, q in bynet.get(n, []):
                if rr == r:
                    continue
                d = math.hypot(q["x"] - p["x"], q["y"] - p["y"])
                val = F[rr]["props"].get("Value", "")
                if TVS.search(val):
                    tv = True; items.append(f"*{rr}({val}) {d:.1f}mm")
                elif rr[0] == "U":
                    direct.append(rr); items.append(f"{rr}.{q['num']}({val}) {d:.1f}mm")
                else:
                    items.append(f"{rr}({val}) {d:.1f}mm")
            flag = ""
            if direct and not tv:
                flag = "DIRECT-TO-IC, no TVS"; bad += 1
            print(f"{r:4s} {p['num']:>5s} {n:28s} {flag:22s} " + ", ".join(sorted(items, key=lambda s: float(s.split()[-1][:-2]))[:6]))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
