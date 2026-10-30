import json,sys,collections
d=json.load(open(sys.argv[1])); c=collections.Counter()
for p in d['parts']:
  c[p.get('verdict')]+=1
  if p.get('verdict')!='ok' or (p.get('offset_mm') or 0)>0.3: print(p['ref'],p.get('lcsc'),p.get('verdict'),p.get('cpl_rot'),p.get('expected_rot'),p.get('offset_mm'),p.get('basis'),p.get('polar'))
print(c, d['status'], d['rate_limited'])
