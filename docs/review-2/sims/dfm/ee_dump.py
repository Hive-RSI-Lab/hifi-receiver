"""dump an LCSC part's EasyEDA pads in mm, centred on the pad-set centre: ee_dump.py C123 ..."""
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1]); 
import easyeda
cache=Path('/work/docs/review-2/sims/dfm/easyeda-cache')
for c in sys.argv[2:]:
    errs={}
    m=easyeda.get(c, cache, fetch=True, errors=errs) if 'errors' in easyeda.get.__code__.co_varnames else easyeda.get(c, cache)
    if m is None: print(c,'NO MODEL',errs); continue
    print('==',c,getattr(m,'title',''),getattr(m,'package',''))
    for p in m.pads: print('  ',p)
