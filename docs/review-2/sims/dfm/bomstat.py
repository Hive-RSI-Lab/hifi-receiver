"""BOM Basic/Extended counts and lowest stock per BOM: bomstat.py BOM.csv... (needs /tmp/ds/dfm/lcsc or live search)"""
import csv,json,sys,subprocess,os
S=os.environ['HIVE_MODULES']+'/hive-hwde/.claude/skills/hwde/scripts/parts_search.py'
cache='/tmp/ds/dfm/lcsc'; os.makedirs(cache,exist_ok=True)
def get(c):
    p=f'{cache}/{c}.json'
    if not os.path.exists(p): subprocess.run([sys.executable,S,'--query',c,'--limit','3','--out',p],capture_output=True)
    try: return [x for x in json.load(open(p))['results'] if x['lcsc']==c][0]
    except Exception: return None
for bom in sys.argv[1:]:
    ext=[];low=[];tht=[]
    for r in csv.DictReader(open(bom)):
        x=get(r['LCSC Part #']); n=len(r['Designator'].split(','))
        if not x: low.append((r['LCSC Part #'],'NOT FOUND')); continue
        if not x['basic']: ext.append(r['LCSC Part #'])
        if x['stock']<1000: low.append((x['lcsc'],x['mpn'],x['stock'],n))
        if 'Plugin' in x['package'] or x['package'] in ('-',): tht.append((x['mpn'],r['Designator']))
    print(bom, 'lines',sum(1 for _ in open(bom))-1,'extended',len(ext),'-> loading fee ~$',3*len(ext),'\n  low stock(<1000):',low,'\n  THT:',tht)
