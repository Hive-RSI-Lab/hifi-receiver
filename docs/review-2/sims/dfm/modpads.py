"""list pads + Edge/Dwgs lines of a .kicad_mod: modpads.py file.kicad_mod"""
import sys, sexpdata
from sexpdata import Symbol
S=lambda x: x.value() if isinstance(x,Symbol) else x
d=sexpdata.loads(open(sys.argv[1]).read())
for e in d:
  if isinstance(e,list) and e and S(e[0])=='pad':
    g=lambda k:[x for x in e if isinstance(x,list) and S(x[0])==k]
    at=g('at')[0]; sz=g('size')[0]; dr=g('drill')
    print('pad',e[1],S(e[2]),S(e[3]),'at',at[1:],'size',sz[1:],'drill',[S(v) for v in dr[0][1:] if not isinstance(v,list)] if dr else '')
  if isinstance(e,list) and e and S(e[0]) in ('fp_line','fp_text'):
    l=[x for x in e if isinstance(x,list) and S(x[0])=='layer']
    if l and l[0][1] in ('Dwgs.User','Cmts.User') : print(S(e[0]),[ (S(x[0]),x[1:]) for x in e if isinstance(x,list) and S(x[0]) in ('start','end','at')], e[2] if S(e[0])=='fp_text' else '')
  if isinstance(e,list) and e and S(e[0])=='property' and e[1] in ('Description',): print('desc',e[2][:200])
