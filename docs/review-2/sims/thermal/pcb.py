"""Minimal KiCad 10 .kicad_pcb parser (footprints, pads, vias, zone fills). Thermal review 2026-10-28."""
import re, math
def parse(text):
    tok = re.compile(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+'); st=[[]]
    for m in tok.finditer(text):
        t=m.group(0)
        if t=='(': st.append([])
        elif t==')': x=st.pop(); st[-1].append(x)
        else: st[-1].append(t[1:-1] if t.startswith('"') else t)
    return st[0][0]
def kids(n,name): return [c for c in n if isinstance(c,list) and c and c[0]==name]
def kid(n,name):
    k=kids(n,name); return k[0] if k else None
def pts_of(n): return [(float(c[1]),float(c[2])) for c in kid(n,'pts')[1:] if c[0]=='xy']
class PCB:
    def __init__(s,path):
        r=parse(open(path).read()); s.fps=[]; s.vias=[]; s.zones=[]
        for c in r[1:]:
            if not isinstance(c,list): continue
            if c[0]=='footprint': s.fps.append(s._fp(c))
            elif c[0]=='via':
                a=kid(c,'at'); s.vias.append(dict(x=float(a[1]),y=float(a[2]),size=float(kid(c,'size')[1]),drill=float(kid(c,'drill')[1]),net=(kid(c,'net') or [0,''])[1]))
            elif c[0]=='zone':
                z=dict(net=(kid(c,'net') or [0,''])[1],fills=[])
                for f in kids(c,'filled_polygon'): z['fills'].append((kid(f,'layer')[1],pts_of(f)))
                s.zones.append(z)
    def _fp(s,c):
        a=kid(c,'at'); x,y=float(a[1]),float(a[2]); rot=float(a[3]) if len(a)>3 else 0
        ref=val=''
        for p in kids(c,'property'):
            if p[1]=='Reference': ref=p[2]
            if p[1]=='Value': val=p[2]
        pads=[]
        for p in kids(c,'pad'):
            pa=kid(p,'at'); px,py=float(pa[1]),float(pa[2]); r=math.radians(-rot)
            sz=kid(p,'size'); dr=kid(p,'drill')
            pads.append(dict(num=p[1],x=x+px*math.cos(r)-py*math.sin(r),y=y+px*math.sin(r)+py*math.cos(r),w=float(sz[1]),h=float(sz[2]),
                net=(kid(p,'net') or [0,''])[1],drill=float(dr[1]) if dr and len(dr)>1 and dr[1]!='oval' else None,layers=kid(p,'layers')[1:]))
        return dict(ref=ref,val=val,lib=c[1],x=x,y=y,rot=rot,layer=kid(c,'layer')[1],pads=pads)
    def fp(s,ref): return next(f for f in s.fps if f['ref']==ref)
