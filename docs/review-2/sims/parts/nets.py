"""nets.py <board.net> <regex> : print nodes (ref,pin,value) on nets matching regex"""
import sys,re,os
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from sx import parse,find,one
n=parse(open(sys.argv[1]).read())
vals={one(c,'ref')[1]:one(c,'value')[1] for c in find(one(n,'components'),'comp')}
for net in find(one(n,'nets'),'net'):
    nm=one(net,'name')[1]
    if re.search(sys.argv[2],nm):
        nodes=[(one(d,'ref')[1],one(d,'pin')[1],vals[one(d,'ref')[1]]) for d in find(net,'node')]
        print(nm, nodes if len(nodes)<25 else f'{len(nodes)} nodes')
