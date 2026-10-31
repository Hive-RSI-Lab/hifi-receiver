"""netpath.py - routed path length from one pad to every other pad of a net
(track graph, vias join layers). usage: netpath.py BOARD NET REF:PAD"""
import sys, math, heapq
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "review-3/sims/layout"))
from pcbgeo import load_board
b = load_board(sys.argv[1]); net = sys.argv[2]; src = tuple(sys.argv[3].split(":"))
key = lambda p: (round(p[0], 2), round(p[1], 2))
G = {}
def edge(a, c, w):
    G.setdefault(a, []).append((c, w)); G.setdefault(c, []).append((a, w))
pts = set()
for t in b["tracks"]:
    if t["net"] == net:
        a, c = key(t["s"]), key(t["e"]); edge(a, c, math.dist(t["s"], t["e"])); pts |= {a, c}
for v in b["vias"]:
    if v.get("net") == net: pts.add(key(v["xy"]))
pads = {}
for r, f in b["footprints"].items():
    for p in f["pads"]:
        if p["net"] == net:
            pk = ("pad", r, p["num"]); pads[(r, p["num"])] = pk
            for q in pts:
                d = math.dist(q, (p["x"], p["y"]))
                if d < 0.6: edge(pk, q, d)
# join coincident points (endpoints within 0.05 mm)
L = sorted(pts)
for i, a in enumerate(L):
    for c in L[i + 1:]:
        if c[0] - a[0] > 0.05: break
        if math.dist(a, c) < 0.05: edge(a, c, 0)
s = pads[src]; D = {s: 0}; h = [(0, s)]
while h:
    d, u = heapq.heappop(h)
    if d > D.get(u, 1e9): continue
    for v, w in G.get(u, []):
        if d + w < D.get(v, 1e9): D[v] = d + w; heapq.heappush(h, (d + w, v))
for k, pk in pads.items():
    print(net, src, "->", k, round(D.get(pk, float("nan")), 1), "mm")
