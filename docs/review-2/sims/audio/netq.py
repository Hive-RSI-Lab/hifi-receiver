"""Print the pins on given nets from a KiCad .net (read-only)."""
import sys, pcbq
t = pcbq.load(sys.argv[1])
nets = pcbq.first(t, 'nets')
want = set(sys.argv[2:])
for n in pcbq.find(nets, 'net'):
    name = pcbq.first(n, 'name')[1]
    if name in want or any(w.endswith('*') and name.startswith(w[:-1]) for w in want):
        pins = [(pcbq.first(nd, 'ref')[1], pcbq.first(nd, 'pin')[1]) for nd in pcbq.find(n, 'node')]
        print(name, pins)
