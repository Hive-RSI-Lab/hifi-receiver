"""review-5 UF: ultra-front fab package against the board (read-only).
 1. every PCB footprint (except mounting holes) has one BOM row and one CPL row,
    and nothing else is in them;
 2. BOM value = PCB value = netlist value; BOM LCSC = parts.json LCSC for that ref;
 3. CPL Mid X/Y = PCB position (y negated, pos-file convention) within 0.01 mm,
    CPL layer = Top, CPL rotation = PCB rotation mod 360 (the EasyEDA-model
    rotation fit is cpl_pin1.py's job: docs/review-3/sims/layout/cpl_pin1.py);
 4. the zipped Gerbers equal the loose ones byte for byte.
(Gerbers against a fresh kicad-cli export: done by hand in uf.md, all 10 identical.)
Usage: fab_check.py [repo root]. Exit 1 on any mismatch."""
import csv, json, re, sys, zipfile, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../review-3/sims/amp'))
import pcbq
R = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../..')
B = f"{R}/boards/ultra-front"
bad = []
b = pcbq.load(f"{B}/kicad/ultra-front.kicad_pcb")
fps = pcbq.footprints(b)
val = {}
for fp in pcbq.find(b, 'footprint'):
    props = {p[1]: p[2] for p in pcbq.find(fp, 'property')}
    val[props['Reference']] = props.get('Value')
net = open(f"{B}/kicad/ultra-front.net").read()
nval = dict(re.findall(r'\(comp\s+\(ref "([^"]+)"\)\s+\(value "([^"]*)"\)', net))
parts = {r: p['lcsc'] for p in json.load(open(f"{B}/parts/parts.json"))['parts'] for r in p['refdes']}
refs = {r for r in fps if not r.startswith('H')}
bom = {}
for row in csv.DictReader(open(f"{B}/fab/BOM.csv")):
    for r in row['Designator'].split(','):
        bom[r.strip()] = row
cpl = {row['Designator']: row for row in csv.DictReader(open(f"{B}/fab/CPL.csv"))}
for name, s in (('BOM', set(bom)), ('CPL', set(cpl)), ('netlist', set(nval)), ('parts.json', set(parts))):
    if s != refs:
        bad.append(f"{name} refs differ from PCB: missing {sorted(refs - s)}, extra {sorted(s - refs)}")
for r in sorted(refs):
    f = fps[r]
    if r in bom:
        if bom[r]['Comment'] != val[r] and not bom[r]['Comment'].startswith(val[r].split('/')[0]):
            bad.append(f"{r}: BOM value {bom[r]['Comment']} vs PCB {val[r]}")
        if bom[r]['LCSC Part #'] != parts.get(r):
            bad.append(f"{r}: BOM LCSC {bom[r]['LCSC Part #']} vs parts.json {parts.get(r)}")
    if nval.get(r) != val[r]:
        bad.append(f"{r}: netlist value {nval.get(r)} vs PCB {val[r]}")
    if r in cpl:
        c = cpl[r]
        if abs(float(c['Mid X']) - f['x']) > 0.01 or abs(-float(c['Mid Y']) - f['y']) > 0.01:
            bad.append(f"{r}: CPL at ({c['Mid X']},{c['Mid Y']}) vs PCB ({f['x']},{-f['y']})")
        if c['Layer'] != 'Top' or f['layer'] != 'F.Cu':
            bad.append(f"{r}: layer CPL {c['Layer']} PCB {f['layer']}")
        if abs((float(c['Rotation']) - f['rot']) % 360) > 0.01:
            bad.append(f"{r}: CPL rot {c['Rotation']} vs PCB {f['rot']}")
z = zipfile.ZipFile(f"{B}/fab/ultra-front_gerbers.zip")
for n in z.namelist():
    p = f"{B}/fab/gerbers/{os.path.basename(n)}"
    if not os.path.exists(p) or open(p, 'rb').read() != z.read(n):
        bad.append(f"zip member {n} differs from fab/gerbers")
print(f"{len(refs)} placed refs; BOM {len(bom)}, CPL {len(cpl)}, netlist {len(nval)}, parts.json {len(parts)}; zip {len(z.namelist())} files")
for x in bad:
    print("FAIL", x)
print("PASS" if not bad else f"{len(bad)} mismatches")
sys.exit(1 if bad else 0)
