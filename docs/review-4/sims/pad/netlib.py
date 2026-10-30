"""Read-only KiCad .net helper for the digital review (round 2).

load(path) -> (comps, nets)
  comps: ref -> {value, footprint, lib, part, pins: {pin: (name, type)}, fields}
  nets:  netname -> [(ref, pin, pinfunction, pintype)]
pin_net(nets) -> {(ref, pin): netname}
"""
import re

def _parse(text):
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

def _find(n, k):
    return [c for c in n if isinstance(c, list) and c and c[0] == k]

def _first(n, k):
    r = _find(n, k); return r[0] if r else None

def _val(n, k, d=None):
    f = _first(n, k)
    return f[1] if f and len(f) > 1 else d

def load(path):
    t = _parse(open(path, encoding='utf-8').read())
    comps = {}
    for c in _find(_first(t, 'components'), 'comp'):
        ref = _val(c, 'ref')
        fields = {}
        fl = _first(c, 'fields')
        if fl:
            for f in _find(fl, 'field'):
                nm = _first(f, 'name')
                fields[nm[1] if nm else '?'] = f[-1] if isinstance(f[-1], str) else ''
        props = {}
        for p in _find(c, 'property'):
            props[_val(p, 'name')] = _val(p, 'value', '')
        comps[ref] = dict(value=_val(c, 'value'), footprint=_val(c, 'footprint'),
                          fields=fields, props=props)
    nets = {}
    for n in _find(_first(t, 'nets'), 'net'):
        name = _val(n, 'name')
        nodes = []
        for nd in _find(n, 'node'):
            nodes.append((_val(nd, 'ref'), _val(nd, 'pin'), _val(nd, 'pinfunction', ''),
                          _val(nd, 'pintype', '')))
        nets[name] = nodes
    return comps, nets

def pin_net(nets):
    out = {}
    for name, nodes in nets.items():
        for ref, pin, fn, ty in nodes:
            out[(ref, pin)] = name
    return out

def short(name):
    return name.rsplit('/', 1)[-1] if name else name
