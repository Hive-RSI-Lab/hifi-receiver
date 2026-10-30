"""Dump footprint pads in board coords: fp.py board.kicad_pcb REF,REF|all"""
import sys, math, sexpdata
from sexpdata import Symbol
def S(x): return x.value() if isinstance(x,Symbol) else x
def find(l,k): return [e for e in l if isinstance(e,list) and e and S(e[0])==k]
def one(l,k):
    r=find(l,k); return r[0] if r else None
data=sexpdata.loads(open(sys.argv[1]).read())
refs=sys.argv[2].split(',') if len(sys.argv)>2 else ['all']
for f in find(data,'footprint'):
    lib=f[1]; at=one(f,'at'); fx,fy=at[1],at[2]; a=at[3] if len(at)>3 else 0
    layer=one(f,'layer')[1]
    r=[p[2] for p in find(f,'property') if p[1]=='Reference'][0]
    if refs!=['all'] and r not in refs: continue
    m=[S(x[1]) for x in find(f,'model')]
    print(f"== {r} {lib} at ({fx:.3f},{fy:.3f}) rot {a} layer {layer} models {m}")
    for p in find(f,'pad'):
        num=p[1]; typ=S(p[2]); shp=S(p[3]); pat=one(p,'at'); x,y=pat[1],pat[2]; pa=pat[3] if len(pat)>3 else 0
        sz=one(p,'size'); dr=one(p,'drill'); net=one(p,'net'); lay=one(p,'layers')
        ca,sa=math.cos(math.radians(a)),math.sin(math.radians(a))
        bx=fx + x*ca + y*sa; by=fy - x*sa + y*ca
        ds=' drill '+' '.join(str(S(v)) for v in dr[1:] if not isinstance(v,list)) if dr else ''
        print(f"  pad {num:>4} {typ:9} {shp:9} sz {sz[1]:.3f}x{sz[2]:.3f}{ds} padrot {pa} local ({x:.3f},{y:.3f}) board ({bx:.3f},{by:.3f}) net {net[-1] if net else ''} {[S(v) for v in lay[1:]][:4]}")
    for g in f:
        if isinstance(g,list) and S(g[0]) in ('fp_line','fp_rect','fp_circle','fp_poly'):
            l=one(g,'layer')
            if l and l[1] in ('F.CrtYd','B.CrtYd'):
                print('  crtyd',S(g[0]),[(S(x[0]),x[1:]) for x in g if isinstance(x,list) and S(x[0]) in ('start','end','center')])
