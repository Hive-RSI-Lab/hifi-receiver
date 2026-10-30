"""Copper of a net around a part, per layer: python copper.py board.kicad_pcb REF NET R_mm"""
import sys; sys.path.insert(0,__import__('os').path.dirname(__file__)); from pcb import PCB
from shapely.geometry import Polygon, Point
from shapely.ops import unary_union
b=PCB(sys.argv[1]); f=b.fp(sys.argv[2]); net=sys.argv[3]; R=float(sys.argv[4])
pp=[p for p in f['pads'] if p['net']==net]
cx=sum(p['x'] for p in pp)/len(pp); cy=sum(p['y'] for p in pp)/len(pp)
disc=Point(cx,cy).buffer(R)
print(f"{f['ref']} {f['val']} net {net}: {len(pp)} pads, centroid ({cx:.2f},{cy:.2f}); vias of net within R: {sum(1 for v in b.vias if v['net']==net and disc.contains(Point(v['x'],v['y'])))}")
for L in sorted(set(l for z in b.zones for l,_ in z['fills'])):
    u=unary_union([Polygon(p).buffer(0) for z in b.zones if z['net']==net for l,p in z['fills'] if l==L and len(p)>2])
    if u.is_empty: print(f"  {L}: none"); continue
    # region connected to the pads
    pads_pts=[Point(p['x'],p['y']) for p in pp]
    geoms=getattr(u,'geoms',[u]); conn=[g for g in geoms if any(g.buffer(0.3).contains(q) for q in pads_pts)]
    cu=unary_union(conn) if conn else Polygon()
    print(f"  {L}: within R{R:.0f} {u.intersection(disc).area:6.0f} mm2 of {disc.area:.0f}; fill touching the pads: {cu.area:6.0f} mm2")
