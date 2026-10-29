"""Append to docs/review-2/parts.json. usage: python3 add.py finding|ku|tool '<json object>'"""
import json, sys, os
J='/work/docs/review-2/parts.json'
d=json.load(open(J)) if os.path.exists(J) else {"domain":"parts","findings":[],"known_unknowns":[],"tools_wanted":[]}
kind=sys.argv[1]; obj=json.loads(sys.argv[2])
if kind=='finding':
    obj={"id":f"PARTS-{len(d['findings'])+1}",**obj}; d['findings'].append(obj); print(obj['id'])
elif kind=='ku':
    obj.setdefault('date','2026-10-28'); d['known_unknowns'].append(obj)
else: d['tools_wanted'].append(obj)
json.dump(d,open(J,'w'),indent=1,ensure_ascii=False)
