import json
F=[];K=[];T=[]
def f(id,board,sev,refs,nets,finding,evidence,fix,margin): F.append(dict(id=id,board=board,severity=sev,refs=refs,nets=nets,finding=finding,evidence=evidence,fix=fix,margin=margin))
def k(fact,board,refs,status,source,stop,date="2026-10-28"): K.append(dict(fact=fact,board=board,refs=refs,status=status,source=source,date=date,could_stop_board=stop))
def t(tool,checks,why): T.append(dict(tool=tool,checks=checks,why=why))
exec(open(__file__.replace('build_json.py','items.py')).read())
json.dump(dict(domain='dfm',findings=F,known_unknowns=K,tools_wanted=T),open('/work/docs/review-2/dfm.json','w'),indent=1,ensure_ascii=False)
print(len(F),len(K),len(T))
