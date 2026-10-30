"""Minimal read-only KiCad .kicad_pcb reader (handles KiCad 9/10 net syntax)."""
import re, math

def parse(text):
    tok = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text)
    st = [[]]
    for t in tok:
        if t == '(':
            st.append([])
        elif t == ')':
            x = st.pop(); st[-1].append(x)
        else:
            st[-1].append(t[1:-1] if t.startswith('"') else t)
    return st[0][0]

def find(n, k): return [c for c in n if isinstance(c, list) and c and c[0] == k]
def first(n, k):
    r = find(n, k); return r[0] if r else None

def netname(node, netmap):
    n = first(node, 'net')
    if not n or len(n) < 2: return ''
    v = n[-1]
    return netmap.get(v, v) if len(n) == 2 else n[2]

class Pcb:
    def __init__(self, path):
        t = parse(open(path, encoding='utf-8').read())
        self.netmap = {n[1]: n[2] for n in find(t, 'net') if len(n) > 2}
        self.fps = {}
        for fp in find(t, 'footprint'):
            ref = next((p[2] for p in find(fp, 'property') if p[1] == 'Reference'), None)
            at = first(fp, 'at'); x, y = float(at[1]), float(at[2]); a = float(at[3]) if len(at) > 3 else 0
            pads = {}
            for p in find(fp, 'pad'):
                pa = first(p, 'at'); px, py = float(pa[1]), float(pa[2])
                r = math.radians(a)
                ax = x + px * math.cos(r) + py * math.sin(r)
                ay = y - px * math.sin(r) + py * math.cos(r)
                sz = first(p, 'size')
                pads.setdefault(p[1], []).append({'xy': (ax, ay), 'net': netname(p, self.netmap),
                                                  'size': (float(sz[1]), float(sz[2])) if sz else None})
            self.fps[ref] = {'xy': (x, y), 'rot': a, 'layer': first(fp, 'layer')[1], 'pads': pads}
        self.segs = []
        for s in find(t, 'segment') + find(t, 'arc'):
            self.segs.append({'a': tuple(map(float, first(s, 'start')[1:3])), 'b': tuple(map(float, first(s, 'end')[1:3])),
                              'w': float(first(s, 'width')[1]), 'layer': first(s, 'layer')[1], 'net': netname(s, self.netmap)})
        self.vias = []
        for v in find(t, 'via'):
            self.vias.append({'xy': tuple(map(float, first(v, 'at')[1:3])), 'net': netname(v, self.netmap)})

def seg_dist(p, a, b):
    ax, ay = a; bx, by = b; px, py = p
    dx, dy = bx - ax, by - ay
    L = dx * dx + dy * dy
    t = 0 if L == 0 else max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / L))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)
