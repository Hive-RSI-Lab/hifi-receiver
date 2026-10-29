"""Tiny KiCad PCB/netlist query helper for the audio review (read-only)."""
import re, sys, math

def parse(text):
    tok = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text)
    stack = [[]]
    for t in tok:
        if t == '(':
            stack.append([])
        elif t == ')':
            x = stack.pop(); stack[-1].append(x)
        else:
            stack[-1].append(t[1:-1] if t.startswith('"') else t)
    return stack[0][0]

def find(node, key):
    return [c for c in node if isinstance(c, list) and c and c[0] == key]

def first(node, key):
    r = find(node, key); return r[0] if r else None

def load(path):
    return parse(open(path, encoding='utf-8').read())

def footprints(board):
    out = {}
    for fp in find(board, 'footprint'):
        ref = None
        for p in find(fp, 'property'):
            if p[1] == 'Reference': ref = p[2]
        at = first(fp, 'at'); layer = first(fp, 'layer')[1]
        x, y = float(at[1]), float(at[2]); rot = float(at[3]) if len(at) > 3 else 0.0
        pads = []
        for pd in find(fp, 'pad'):
            pat = first(pd, 'at'); sz = first(pd, 'size'); net = first(pd, 'net')
            px, py = float(pat[1]), float(pat[2])
            r = math.radians(-rot)
            gx = x + px*math.cos(r) - py*math.sin(r); gy = y + px*math.sin(r) + py*math.cos(r)
            netname = None
            if net:
                netname = net[-1] if len(net) > 1 else None
            pads.append(dict(num=pd[1], kind=pd[2], shape=pd[3], x=gx, y=gy, lx=px, ly=py,
                             w=float(sz[1]), h=float(sz[2]), net=netname))
        out[ref] = dict(lib=fp[1], x=x, y=y, rot=rot, layer=layer, pads=pads)
    return out
