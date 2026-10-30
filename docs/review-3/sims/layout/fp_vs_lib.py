#!/usr/bin/env python3
"""fp_vs_lib.py - does each footprint on the board match its library .kicad_mod? (review-3 LAYOUT)

Round 1's RCA fix (DFM-1/2) went into core's library footprint but never onto
the board. KiCad's own lib_footprint_mismatch DRC is set to 'ignore' in the
projects, so nothing caught it. This compares, per footprint instance, the pad
set (number, type, local x/y, size, drill) with the library file
<board>/lib/<nick>.pretty/<name>.kicad_mod. Exit 1 on any difference.

Usage: fp_vs_lib.py BOARD.kicad_pcb LIBROOT   (LIBROOT = boards/<b>/lib)
"""
import re, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pcbgeo import _parse, find, first


def pads_of(node):
    out = []
    for pd in find(node, "pad"):
        at = first(pd, "at"); sz = first(pd, "size"); dr = first(pd, "drill")
        drill = None
        if dr:
            nums = [x for x in dr[1:] if not isinstance(x, list) and x != "oval"]
            drill = tuple(round(float(x), 2) for x in nums)
        out.append((pd[1], pd[2], round(float(at[1]), 2), round(float(at[2]), 2),
                    round(float(sz[1]), 2), round(float(sz[2]), 2), drill))
    return sorted(out)


def main():
    pcb, libroot = sys.argv[1], Path(sys.argv[2])
    b = _parse(open(pcb, encoding="utf-8").read())
    bad = 0; n = 0; missing = set()
    cache = {}
    for fp in find(b, "footprint"):
        ref = next((p[2] for p in find(fp, "property") if p[1] == "Reference"), "?")
        nick, _, name = fp[1].partition(":")
        f = libroot / f"{nick}.pretty" / f"{name}.kicad_mod"
        if not f.exists():
            missing.add(fp[1]); continue
        if f not in cache:
            cache[f] = pads_of(_parse(f.read_text(encoding="utf-8")))
        n += 1
        lib, brd = cache[f], pads_of(fp)
        if lib != brd:
            bad += 1
            only_lib = [p for p in lib if p not in brd]; only_brd = [p for p in brd if p not in lib]
            print(f"MISMATCH {ref} {fp[1]}: library-only {only_lib[:4]} board-only {only_brd[:4]}")
    print(f"{pcb}: {n} footprints compared, {bad} mismatched; not in {libroot}: {sorted(missing)[:8]}{' ...' if len(missing) > 8 else ''}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
