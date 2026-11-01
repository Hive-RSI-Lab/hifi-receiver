"""bottom_fit.py - which rotation of the LCSC EasyEDA model, seen from the
bottom (model mirrored in x), lands each pad on the board pad of the same
number, for the bottom-side polarised parts (D6-D9). Prints the fitting
rotations and the CPL's. JLC's bottom-side rotation convention decides which
CPL value is right; this gives the number to check in JLC's preview.
usage: bottom_fit.py BOARD CPL CACHE_DIR"""
import csv, json, math, sys
from pathlib import Path
L = Path(__file__).resolve().parents[4] / "review-3/sims/layout"
sys.path.insert(0, str(L))
from pcbgeo import load_board
from cpl_pin1 import model_pads, rot
b = load_board(sys.argv[1])["footprints"]
rows = {r["Designator"]: r for r in csv.DictReader(open(sys.argv[2]))}
bad = 0
for ref in ("D6", "D7", "D8", "D9"):
    fp = b[ref]; row = rows[ref]
    l = fp["props"].get("LCSC")
    m = json.load(open(Path(sys.argv[3]) / f"{l}.json"))
    mp = model_pads(m)  # {num: (x, y, w, h)} relative, y down
    pads = {p["num"]: (p["x"] - fp["x"], p["y"] - fp["y"]) for p in fp["pads"]}
    fits = {}
    for mirror in (False, True):
        for deg in range(0, 360, 90):
            err = 0
            for n, v in mp.items():
                if n not in pads: continue
                x, y = v[0][0], v[0][1]
                if mirror: x = -x
                rx, ry = rot(x, y, deg)
                err = max(err, math.dist((rx, ry), pads[n]))
            fits[(mirror, deg)] = round(err, 2)
    ok = [k for k, e in fits.items() if e < 0.2]
    print(ref, l, "board rot", fp["rot"], "CPL", row["Layer"], row["Rotation"], "fits (mirrored, deg):", ok)
