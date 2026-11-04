"""Read-only KiCad .net reader for the power review (review-3)."""
import re

def _parse(text):
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

def _find(n, k):
    return [c for c in n if isinstance(c, list) and c and c[0] == k]

def _first(n, k):
    r = _find(n, k); return r[0] if r else None

class Net:
    def __init__(self, path):
        t = _parse(open(path, encoding='utf-8').read())
        self.comp = {}
        for c in _find(_first(t, 'components'), 'comp'):
            ref = _first(c, 'ref')[1]
            val = _first(c, 'value')[1] if _first(c, 'value') else ''
            fields = {}
            fl = _first(c, 'fields')
            if fl:
                for f in _find(fl, 'field'):
                    nm = _first(f, 'name')
                    if nm and len(f) > 2:
                        fields[nm[1]] = f[-1] if isinstance(f[-1], str) else ''
            props = {p[1][1]: p[2][1] for p in _find(c, 'property') if len(p) > 2 and isinstance(p[2], list)}
            self.comp[ref] = {'value': val, 'fields': fields, 'props': props}
        self.nets = {}
        self.pin = {}
        for n in _find(_first(t, 'nets'), 'net'):
            name = _first(n, 'name')[1]
            nodes = [(_first(d, 'ref')[1], _first(d, 'pin')[1]) for d in _find(n, 'node')]
            self.nets[name] = nodes
            for r, p in nodes:
                self.pin[(r, p)] = name

    def norm(self, name):
        return name.lstrip('/').split('/')[-1]

    def netof(self, ref, pin):
        n = self.pin.get((ref, str(pin)))
        return self.norm(n) if n else None

    def pins_of(self, ref):
        return {p: self.norm(n) for (r, p), n in self.pin.items() if r == ref}

    def on(self, net):
        out = []
        for n, nodes in self.nets.items():
            if self.norm(n) == net:
                out += nodes
        return out

    def refs_on(self, net):
        return sorted({r for r, _ in self.on(net)})

    def value(self, ref):
        return self.comp[ref]['value']

    def between(self, a, b):
        """Two-pin parts with one pin on net a and the other on net b."""
        out = []
        for ref in self.comp:
            ps = self.pins_of(ref)
            if len(ps) == 2 and sorted(ps.values()) == sorted([a, b]):
                out.append(ref)
        return sorted(out)
