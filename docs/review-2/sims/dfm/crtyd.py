"""courtyard vs pad extent for every footprint on a board (local coords, axis-aligned bbox)"""
import sys, sexpdata
from sexpdata import Symbol
def S(x): return x.value() if isinstance(x,Symbol) else x
def one(l,k):
    r=[e for e in l if isinstance(e,list) and e and S(e[0])==k]; return r[0] if r else None
def check(f):
    fa=one(f,'at'); fa=fa[3] if len(fa)>3 else 0
    xs=[];ys=[]
    for g in f:
        if isinstance(g,list) and S(g[0]) in ('fp_line','fp_rect','fp_poly','fp_circle'):
            l=one(g,'layer')
            if l and l[1] in ('F.CrtYd','B.CrtYd'):
                if S(g[0])=='fp_circle':
                    c=one(g,'center'); e=one(g,'end'); r=((c[1]-e[1])**2+(c[2]-e[2])**2)**.5
                    xs+= [c[1]-r,c[1]+r]; ys+=[c[2]-r,c[2]+r]
                elif S(g[0])=='fp_poly':
                    for p in one(g,'pts')[1:]: xs.append(p[1]); ys.append(p[2])
                else:
                    for k in ('start','end'): p=one(g,k); xs.append(p[1]); ys.append(p[2])
    if not xs: return None
    worst=0; bad=[]
    for p in f:
        if isinstance(p,list) and S(p[0])=='pad':
            at=one(p,'at'); sz=one(p,'size'); rot=((at[3] if len(at)>3 else 0)-fa)%180
            w,h=(sz[1],sz[2]) if abs(rot)<1 else (sz[2],sz[1])
            ex=max(min(xs)-(at[1]-w/2),(at[1]+w/2)-max(xs),min(ys)-(at[2]-h/2),(at[2]+h/2)-max(ys))
            if ex>0.01: bad.append((p[1],round(ex,3)))
    return bad
d=sexpdata.loads(open(sys.argv[1]).read())
import collections
res=collections.defaultdict(list)
for f in d:
    if isinstance(f,list) and f and S(f[0])=='footprint':
        r=[p[2] for p in f if isinstance(p,list) and S(p[0])=='property' and p[1]=='Reference'][0]
        b=check(f)
        if b: res[f[1]].append((r,max(x[1] for x in b)))
for k,v in sorted(res.items()): print(k,len(v),'max over',max(x[1] for x in v),' '.join(x[0] for x in v))
print('footprints with pads outside courtyard:',sum(len(v) for v in res.values()))
