"""q.py - quick PCB query: parts on nets, pad positions, track length per layer.
usage: q.py BOARD NETSUBSTR[,..]"""
import sys, math
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "review-3/sims/layout"))
from pcbgeo import load_board
b = load_board(sys.argv[1])
for sub in sys.argv[2].split(","):
    nets = sorted({p["net"] for f in b["footprints"].values() for p in f["pads"] if p["net"] and p["net"].endswith(sub)})
    for n in nets:
        pads = [(r, p["num"], round(p["x"],2), round(p["y"],2), f.get("layer")) for r, f in b["footprints"].items() for p in f["pads"] if p["net"] == n]
        L = {}
        for t in b["tracks"]:
            if t["net"] == n:
                L[t["layer"]] = L.get(t["layer"], 0) + math.dist(t["s"], t["e"])
        nv = sum(1 for v in b["vias"] if v["net"] == n)
        print(n, {k: round(v,1) for k,v in L.items()}, "vias", nv)
        for x in pads: print("   ", x)
