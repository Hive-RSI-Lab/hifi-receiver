"""Own Gerber read (gerbonara+shapely): drill table, PTH annular rings, mask webs, silk on mask openings.
usage: gerb.py <dir with extracted gerbers+drl>"""
import sys, math, collections, json
from pathlib import Path
import gerbonara as gb
from gerbonara import LayerStack
from gerbonara.graphic_objects import Flash, Line, Arc, Region
from shapely.geometry import Point, Polygon, box, LineString
from shapely.ops import unary_union
from shapely import affinity
from shapely.strtree import STRtree
d=Path(sys.argv[1]); ls=LayerStack.open(d)
drills=[]
for dl in ls.drill_layers:
    for o in dl.objects:
        if isinstance(o, Flash): drills.append(('hole', o.x, o.y, o.aperture.diameter))
        elif isinstance(o, Line): drills.append(('slot', (o.x1+o.x2)/2, (o.y1+o.y2)/2, o.aperture.diameter))
for k,v in sorted(collections.Counter((k[0], round(k[3],3)) for k in drills).items(), key=lambda x:x[0][1]): print('drill',k,v)
def prim(p):
    n=type(p).__name__
    if n=='Circle': return Point(p.x,p.y).buffer(p.r, 32)
    if n=='Rectangle': return affinity.rotate(box(p.x-p.w/2,p.y-p.h/2,p.x+p.w/2,p.y+p.h/2), p.rotation, origin=(p.x,p.y), use_radians=True)
    if n in ('Line','Arc'): return LineString([(p.x1,p.y1),(p.x2,p.y2)]).buffer(p.width/2,16)
    if n=='ArcPoly':
        try: return Polygon(list(p.outline)).buffer(0)
        except Exception: return None
def shapes(layer, kinds=(Flash,Line,Arc,Region)):
    out=[]
    for o in layer.objects:
        if isinstance(o,kinds):
            try: sh=[prim(p) for p in o.to_primitives(gb.utils.MM)]
            except Exception: continue
            sh=[s for s in sh if s is not None and not s.is_empty]
            if sh: out.append(unary_union(sh))
    return out
for side in ('top','bottom'):
    cu=ls.graphic_layers.get((side,'copper')); fl=shapes(cu,(Flash,)); tree=STRtree(fl); rings=collections.Counter()
    for h in drills:
        if h[0]!='hole': continue
        pt=Point(h[1],h[2]); best=None
        for i in tree.query(pt.buffer(h[3]/2+0.01)):
            s=fl[i]
            if s.contains(pt):
                r=s.exterior.distance(pt)-h[3]/2
                best=r if best is None or r>best else best
        rings[(round(best,3) if best is not None else 'no-cu', h[3])]+=1
    print(side,'annular ring (ring,drill):count',sorted(rings.items(),key=lambda x:str(x[0])))
    mk=ls.graphic_layers.get((side,'mask')); u=unary_union(shapes(mk)); geoms=list(getattr(u,'geoms',[u])); t=STRtree(geoms); webs=[]
    for i,g in enumerate(geoms):
        for j in t.query(g.buffer(0.2)):
            if j>i:
                dd=g.distance(geoms[j])
                if dd<0.2: webs.append(round(dd,3))
    print(side,'mask openings',len(geoms),'webs<0.20 by width',sorted(collections.Counter(webs).items())[:10])
    sk=ls.graphic_layers.get((side,'silk'))
    if sk:
        inter=unary_union(shapes(sk)).intersection(u)
        print(side,'silk on mask openings mm2',round(inter.area,3),[(round(g.centroid.x,2),round(g.centroid.y,2)) for g in getattr(inter,'geoms',[inter]) if not g.is_empty][:10])
