"""Per-ref chain dump: symbol pin (num,name) -> netlist net -> PCB pad (pos rel. footprint, size, PCB net).
usage: python chain.py board.net board.kicad_pcb out.txt   (parts review, docs/review-2/parts.md)"""
import sys, re, os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from sx import parse, find, one
net=parse(open(sys.argv[1]).read()); pcb=parse(open(sys.argv[2]).read()); out=sys.argv[3]
comps={}
for c in find(one(net,'components'),'comp'):
    ref=one(c,'ref')[1]; ls=one(c,'libsource')
    comps[ref]=dict(value=one(c,'value')[1], lib=one(ls,'lib')[1], part=one(ls,'part')[1])
libparts={}
for lp in find(one(net,'libparts'),'libpart'):
    k=(one(lp,'lib')[1],one(lp,'part')[1]); pins={}
    ps=one(lp,'pins')
    if ps:
        for p in find(ps,'pin'): pins[one(p,'num')[1]]=(one(p,'name')[1],one(p,'type')[1])
    libparts[k]=pins
pinnet={}
for n in find(one(net,'nets'),'net'):
    name=one(n,'name')[1]
    for nd in find(n,'node'): pinnet[(one(nd,'ref')[1],one(nd,'pin')[1])]=name
pads={}; fpinfo={}
for fp in find(pcb,'footprint'):
    props={p[1]:p[2] for p in find(fp,'property')}
    ref=props.get('Reference'); at=one(fp,'at')
    fpinfo[ref]=(fp[1],one(fp,'layer')[1],at[1:])
    for p in find(fp,'pad'):
        pa=one(p,'at'); sz=one(p,'size'); n=one(p,'net')
        pads.setdefault(ref,[]).append((p[1],p[2],p[3],float(pa[1]),float(pa[2]),sz[1],sz[2],n[-1] if n else ''))
skip=re.compile(sys.argv[4] if len(sys.argv)>4 else r'^(R|C|H)\d')
with open(out,'w') as f:
  for ref in sorted(comps,key=lambda r:(re.sub(r'\d','',r),int(re.sub(r'\D','',r) or 0))):
    if skip.match(ref): continue
    c=comps[ref]; pins=libparts.get((c['lib'],c['part']),{})
    fi=fpinfo.get(ref,('MISSING','',''))
    f.write(f"== {ref} {c['value']} sym={c['lib']}:{c['part']} fp={fi[0]} layer={fi[1]} at={fi[2]}\n")
    pl=pads.get(ref,[])
    for num in sorted(pins,key=lambda s:(len(s),s)):
        nm,ty=pins[num]; nn=pinnet.get((ref,num),'<NO NET>')
        mp=[p for p in pl if p[0]==num]
        flag='' if mp else '  !!NO PAD'
        geo='; '.join(f"({p[3]:.3f},{p[4]:.3f}) {p[5]}x{p[6]} {p[1]} {p[2]}" + ('' if p[7]==nn or (nn.startswith('unconnected') and p[7]=='') else f' !!PCBNET={p[7]}') for p in mp)
        f.write(f"  {num:>4} {nm:<14} {nn:<26} {geo}{flag}\n")
    for p in pl:
        if p[0] not in pins: f.write(f"  pad {p[0]!r} not in symbol: net={p[7]} ({p[3]:.3f},{p[4]:.3f}) {p[5]}x{p[6]} {p[1]}\n")
