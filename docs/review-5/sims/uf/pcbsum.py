"""ultra-front PCB summary (review-5 UF): per-net routed length/width/layers/vias, zones.
Read-only. Usage: pcbsum.py BOARD.kicad_pcb"""
import math, re, sys
from collections import defaultdict
t = open(sys.argv[1], encoding='utf-8').read()
nets = defaultdict(lambda: {'len': 0.0, 'w': set(), 'layers': set(), 'vias': 0})
for m in re.finditer(r'\(segment\s+\(start ([-\d.]+) ([-\d.]+)\)\s+\(end ([-\d.]+) ([-\d.]+)\)\s+\(width ([\d.]+)\)\s+\(layer "([^"]+)"\)\s+\(net (?:\d+ )?"?([^")]*)"?\)', t):
    x1, y1, x2, y2, w, l, n = m.groups()
    d = nets[n]; d['len'] += math.hypot(float(x2) - float(x1), float(y2) - float(y1)); d['w'].add(float(w)); d['layers'].add(l)
for m in re.finditer(r'\(via\b.*?\(net (?:\d+ )?"?([^")]*)"?\)', t, re.S):
    nets[m.group(1)]['vias'] += 1
for n, d in sorted(nets.items()):
    print(f"{n:22s} {d['len']:7.1f} mm  w {sorted(d['w'])}  {sorted(d['layers'])}  vias {d['vias']}")
for m in re.finditer(r'\(zone\s+\(net (?:\d+ )?"?([^")]*)"?\)(?:\s+\(net_name "[^"]*"\))?\s+\(layers? ([^)]*)\)', t):
    print('zone', m.groups())
print('filled_polygons', t.count('(filled_polygon'))
