#!/usr/bin/env python3
"""Review-5 (dal) fab-package check for boards/core, Plus and Ultra.

Read-only against the repo. Writes fresh Gerbers/drill to /tmp/dal_fab/ only.
Exit status 1 if any check FAILs (WARN lines do not fail).

  python3 docs/review-5/sims/dal/fab/check_fab.py [--no-gerber]

Checks
  1 Gerbers+drill re-exported from core.kicad_pcb vs fab/gerbers/, and
    core_gerbers.zip vs fab/gerbers/.
  2 CPL (Plus, Ultra) vs PCB footprints: refs, X/Y (CPL Y = -PCB Y), layer,
    rotation (deltas reported grouped, plus the bom_cpl rotation audit).
  3 BOM (Plus, Ultra) vs PCB value/footprint, parts DB LCSC, generator LCSC,
    variant membership (DNP / Ultra-only / Plus-only), new parts detail.
  4 Bottom-side parts.
  5 Totals.
"""
from __future__ import annotations

import collections
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
CORE = ROOT / "boards/core"
PCB = CORE / "kicad/core.kicad_pcb"
FAB = CORE / "fab"
GEN = CORE / "kicad/gen"
KCLI = Path(os.environ.get("HWDE_KICAD_CLI",
            Path.home() / "tools/hive-tools/kicad-10.0.6/bin/kicad-cli"))
TMP = Path("/tmp/dal_fab")
TOL = 0.01

FAILS: list[str] = []
WARNS: list[str] = []


def out(s=""):
    print(s, flush=True)


def fail(msg):
    FAILS.append(msg)
    out(f"  FAIL: {msg}")


def warn(msg):
    WARNS.append(msg)
    out(f"  WARN: {msg}")


def ok(msg):
    out(f"  PASS: {msg}")


# ------------------------------------------------------------ s-expr parsing
TOK = re.compile(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()"]+')


def parse_sexpr(text):
    stack = [[]]
    for m in TOK.finditer(text):
        t = m.group(0)
        if t == "(":
            stack.append([])
        elif t == ")":
            x = stack.pop()
            stack[-1].append(x)
        elif t[0] == '"':
            stack[-1].append(t[1:-1].replace('\\"', '"'))
        else:
            stack[-1].append(t)
    return stack[0][0]


def child(node, name):
    for c in node:
        if isinstance(c, list) and c and c[0] == name:
            return c
    return None


def pcb_footprints():
    tree = parse_sexpr(PCB.read_text())
    fps = {}
    for n in tree:
        if not (isinstance(n, list) and n and n[0] == "footprint"):
            continue
        lib = n[1]
        layer = child(n, "layer")[1]
        at = child(n, "at")
        x, y = float(at[1]), float(at[2])
        rot = float(at[3]) if len(at) > 3 and not isinstance(at[3], list) else 0.0
        props = {c[1]: c[2] for c in n if isinstance(c, list) and c and c[0] == "property"}
        attr = child(n, "attr")
        attrs = set(attr[1:]) if attr else set()
        ref = props.get("Reference")
        fps[ref] = dict(ref=ref, lib=lib, fpname=lib.split(":", 1)[-1], layer=layer,
                        x=x, y=y, rot=rot % 360.0, value=props.get("Value"),
                        lcsc=props.get("LCSC"), lcsc_part=props.get("LCSC Part"),
                        attrs=attrs)
    return fps


# ------------------------------------------------------------ helpers
def read_csv(p):
    with open(p, newline="") as fh:
        return list(csv.DictReader(fh))


def split_refs(s):
    return [r.strip() for r in s.split(",") if r.strip()]


def refkey(r):
    m = re.match(r"([A-Za-z]+)(\d+)", r)
    return (m.group(1), int(m.group(2))) if m else (r, 0)


def fmt_refs(refs):
    return ",".join(sorted(refs, key=refkey)) or "-"


def parts_db(path):
    d = json.loads(Path(path).read_text())
    m = {}
    for p in d["parts"]:
        for r in p["refdes"]:
            m[r] = dict(lcsc=p["lcsc"], value=p["value"], mpn=p["mpn"],
                        cls=p.get("refdes_class", {}).get(r, "smt_placed"),
                        desc=p.get("description", ""), package=p.get("package"),
                        basic=p.get("basic"), role=p.get("role", ""))
    return m


def generator_parts():
    # import in a subprocess so nothing leaks; design.parts() is side-effect free
    code = ("import sys,json; sys.path.insert(0, sys.argv[1]); import design;"
            "print(json.dumps([{k:p[k] for k in ('ref','value','lcsc','dnp','plus_only','fp','role')}"
            " for p in design.parts()]))")
    cp = subprocess.run([sys.executable, "-c", code, str(GEN)], capture_output=True,
                        text=True, cwd="/tmp")
    if cp.returncode:
        raise RuntimeError(cp.stderr)
    return {p["ref"]: p for p in json.loads(cp.stdout)}


def norm_val(v):
    return (v or "").replace("µ", "u").replace("μ", "u").strip()


# ------------------------------------------------------------ check 1
LAYERS = "F.Cu,In1.Cu,In2.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts"
IGNORE = re.compile(r'^(G04 |%TF\.CreationDate|%TF\.GenerationSoftware|; |;DRILL file|'
                    r'.*"CreationDate"|.*"GenerationSoftware"|.*"Vendor"|.*"Application"|.*"Version")')


def norm_lines(data: bytes):
    return [l for l in data.decode("utf-8", "replace").splitlines() if not IGNORE.match(l)]


def check_gerbers(skip):
    out("\n[1] Gerbers + drill vs current PCB; zip vs gerbers/")
    gdir = FAB / "gerbers"
    if not skip:
        if TMP.exists():
            shutil.rmtree(TMP)
        fresh = TMP / "gerbers"
        fresh.mkdir(parents=True)
        for args in (["pcb", "export", "gerbers", "-o", str(fresh), "--layers", LAYERS, str(PCB)],
                     ["pcb", "export", "drill", "-o", str(fresh) + "/", "--format", "excellon", str(PCB)]):
            cp = subprocess.run([str(KCLI)] + args, capture_output=True, text=True)
            if cp.returncode:
                fail(f"kicad-cli {' '.join(args[:3])} rc={cp.returncode}: {cp.stderr.strip()[:300]}")
                return
        fab_names = {p.name for p in gdir.iterdir() if p.is_file()}
        new_names = {p.name for p in fresh.iterdir() if p.is_file()}
        if fab_names != new_names:
            fail(f"file sets differ: only in fab {sorted(fab_names - new_names)}, "
                 f"only in fresh {sorted(new_names - fab_names)}")
        bad = 0
        for name in sorted(fab_names & new_names):
            a = norm_lines((gdir / name).read_bytes())
            b = norm_lines((fresh / name).read_bytes())
            if a == b:
                fa = sum(1 for l in a if l.endswith("D03*"))
                out(f"    {name:28s} identical ({len(a)} lines, {fa} flashes)")
                continue
            bad += 1
            ca, cb = collections.Counter(a), collections.Counter(b)
            only_a = sum((ca - cb).values())
            only_b = sum((cb - ca).values())
            fa = sum(1 for l in a if l.endswith("D03*"))
            fb = sum(1 for l in b if l.endswith("D03*"))
            fail(f"{name}: {only_a} lines only in fab, {only_b} only in fresh "
                 f"(of {len(a)}/{len(b)}); flashes fab {fa} vs fresh {fb}")
        if not bad:
            ok(f"{len(fab_names)} files: fresh export == fab/gerbers (headers/timestamps ignored)")
    # zip vs dir
    zp = FAB / "core_gerbers.zip"
    with zipfile.ZipFile(zp) as zf:
        znames = {Path(i.filename).name: zf.read(i) for i in zf.infolist() if not i.is_dir()}
    dnames = {p.name: p.read_bytes() for p in gdir.iterdir() if p.is_file()}
    diff = sorted(n for n in set(znames) | set(dnames) if znames.get(n) != dnames.get(n))
    if diff:
        fail(f"core_gerbers.zip != gerbers/: {diff}")
    else:
        ok(f"core_gerbers.zip == gerbers/ byte-for-byte ({len(znames)} files)")


# ------------------------------------------------------------ check 2
def check_cpl(fps, variant, cpl_path, db, audit):
    out(f"\n[2] CPL vs PCB ({variant}: {cpl_path.relative_to(ROOT)})")
    rows = read_csv(cpl_path)
    cpl = {r["Designator"]: r for r in rows}
    dup = [r for r, n in collections.Counter(r["Designator"] for r in rows).items() if n > 1]
    if dup:
        fail(f"{variant}: duplicate CPL refs {fmt_refs(dup)}")
    # expected: on PCB, not board_only / exclude_from_bom / exclude_from_pos, and
    # placed in this variant per the parts DB (the PCB's own dnp attr is the
    # Plus default only, so the DB decides the variant)
    excl = {r for r, f in fps.items()
            if {"board_only", "exclude_from_bom", "exclude_from_pos_files"} & f["attrs"]}
    exp = {r for r in fps if r not in excl and db.get(r, {}).get("cls") in ("smt_placed", "tht")}
    nodb = sorted(r for r in fps if r not in excl and r not in db)
    if nodb:
        fail(f"{variant}: PCB refs absent from parts DB: {fmt_refs(nodb)}")
    miss = exp - set(cpl)
    extra = set(cpl) - exp
    (fail if miss else ok)(f"{variant}: PCB placed refs missing from CPL: {fmt_refs(miss)}")
    (fail if extra else ok)(f"{variant}: CPL refs not expected (not on PCB / DNP / excluded): "
                            f"{fmt_refs(extra)}")
    xy_bad, lay_bad = [], []
    rot_groups = collections.defaultdict(list)
    for r in sorted(set(cpl) & set(fps), key=refkey):
        f, c = fps[r], cpl[r]
        cx, cy = float(c["Mid X"]), float(c["Mid Y"])
        if abs(cx - f["x"]) > TOL or abs(cy - (-f["y"])) > TOL:
            xy_bad.append(f"{r}(pcb {f['x']:.3f},{-f['y']:.3f} cpl {cx:.3f},{cy:.3f})")
        want = "Bottom" if f["layer"] == "B.Cu" else "Top"
        if c["Layer"] != want:
            lay_bad.append(f"{r}(pcb {f['layer']} cpl {c['Layer']})")
        corr = audit.get(r, {}).get("correction", 0.0) or 0.0
        d = round((float(c["Rotation"]) - f["rot"]) % 360.0, 3) % 360.0
        rot_groups[(c["Layer"], d, round(corr, 3))].append(r)
    (fail if xy_bad else ok)(f"{variant}: X/Y within {TOL} mm (CPL Y = -PCB Y, origin = page origin): "
                             f"{len(xy_bad)} off {' '.join(xy_bad[:20])}")
    (fail if lay_bad else ok)(f"{variant}: Layer matches PCB side: {len(lay_bad)} off {' '.join(lay_bad)}")
    out(f"    rotation (CPL - PCB) mod 360, grouped by layer / delta / bom_cpl audit correction:")
    unexplained = []
    for (lay, d, corr), refs in sorted(rot_groups.items()):
        out(f"      {lay:6s} delta {d:7.2f}  audit-corr {corr:7.2f}  n={len(refs):3d}  "
            f"{fmt_refs(refs) if len(refs) <= 30 else fmt_refs(refs[:30]) + ',...'}")
        if abs(((d - corr + 180) % 360) - 180) > 0.01:
            unexplained += refs
    (fail if unexplained else ok)(f"{variant}: rotation delta == bom_cpl audit correction for all "
                                  f"parts; unexplained: {fmt_refs(unexplained)}")
    return cpl


def check_pos(fps, path):
    """raw kicad pos files kept in fab/: compare to PCB (info)."""
    rows = read_csv(path)
    refs = {r["Ref"] for r in rows}
    allp = {r for r, f in fps.items() if "exclude_from_pos_files" not in f["attrs"]}
    bad = [r["Ref"] for r in rows if r["Ref"] in fps and (
        abs(float(r["PosX"]) - fps[r["Ref"]]["x"]) > TOL or abs(float(r["PosY"]) + fps[r["Ref"]]["y"]) > TOL)]
    msg = (f"{path.relative_to(ROOT)}: {len(rows)} rows; PCB refs not in it {fmt_refs(allp - refs)}; "
           f"not on PCB {fmt_refs(refs - set(fps))}; XY off {fmt_refs(bad)}")
    (warn if (allp - refs) or (refs - set(fps)) or bad else ok)(msg)


# ------------------------------------------------------------ check 3
def check_bom(fps, variant, bom_path, full_path, db, gen, cpl):
    out(f"\n[3] BOM vs PCB / parts DB / generator ({variant}: {bom_path.relative_to(ROOT)})")
    rows = read_csv(bom_path)
    bom = {}
    dups = []
    for row in rows:
        for r in split_refs(row["Designator"]):
            if r in bom:
                dups.append(r)
            bom[r] = row
    if dups:
        fail(f"{variant}: refs on more than one BOM line: {fmt_refs(dups)}")
    lc = collections.Counter(r["LCSC Part #"] for r in rows)
    duplc = [k for k, n in lc.items() if n > 1]
    (fail if duplc else ok)(f"{variant}: one BOM line per LCSC number (dupes: {duplc or '-'})")
    (fail if set(bom) != set(cpl) else ok)(
        f"{variant}: BOM designators == CPL designators "
        f"(BOM-only {fmt_refs(set(bom) - set(cpl))}, CPL-only {fmt_refs(set(cpl) - set(bom))})")
    notpcb, val_bad, fp_bad, db_bad, gen_bad, pcbprop = [], [], [], [], [], []
    val_grp, pcbpart = [], []
    for r, row in sorted(bom.items(), key=lambda kv: refkey(kv[0])):
        f = fps.get(r)
        if not f:
            notpcb.append(r)
            continue
        if norm_val(row["Comment"]) != norm_val(f["value"]):
            # bom_cpl groups one line per LCSC number and the line's Comment is
            # the first ref's value: a different label on a same-LCSC ref is a
            # grouping artefact (WARN); anything else is a real mismatch (FAIL)
            same_part = (db.get(r, {}).get("lcsc") == row["LCSC Part #"]
                         and db.get(r, {}).get("mpn") == db.get(split_refs(row["Designator"])[0], {}).get("mpn"))
            (val_grp if same_part else val_bad).append(f"{r}(bom {row['Comment']} pcb {f['value']})")
        if row["Footprint"] != f["fpname"]:
            fp_bad.append(f"{r}(bom {row['Footprint']} pcb {f['fpname']})")
        lcsc = row["LCSC Part #"]
        if db.get(r, {}).get("lcsc") != lcsc:
            db_bad.append(f"{r}(bom {lcsc} db {db.get(r, {}).get('lcsc')})")
        g = gen.get(r)
        if not g or g["lcsc"] != lcsc:
            gen_bad.append(f"{r}(bom {lcsc} gen {g and g['lcsc']})")
        if f["lcsc"] != lcsc:
            pcbprop.append(f"{r}(bom {lcsc} pcb LCSC={f['lcsc']})")
        if f["lcsc_part"] != lcsc:
            pcbpart.append(f"{r}(bom {lcsc} pcb 'LCSC Part'={f['lcsc_part']})")
    (fail if notpcb else ok)(f"{variant}: BOM refs not on PCB: {fmt_refs(notpcb)}")
    (fail if val_bad else ok)(f"{variant}: BOM Comment == PCB Value: {len(val_bad)} off {' '.join(val_bad)}")
    (fail if fp_bad else ok)(f"{variant}: BOM Footprint == PCB footprint name: {len(fp_bad)} off {' '.join(fp_bad)}")
    (fail if db_bad else ok)(f"{variant}: BOM LCSC == parts DB: {len(db_bad)} off {' '.join(db_bad)}")
    (fail if gen_bad else ok)(f"{variant}: BOM LCSC == generator lcsc=: {len(gen_bad)} off {' '.join(gen_bad)}")
    if val_grp:
        warn(f"{variant}: BOM Comment is another ref's label on a same-LCSC/MPN line "
             f"(grouping artefact, same part): {' '.join(val_grp)}")
    (fail if pcbprop else ok)(f"{variant}: PCB footprint 'LCSC' property == BOM LCSC: "
                              f"{len(pcbprop)} off {' '.join(pcbprop)}")
    if pcbpart:
        warn(f"{variant}: PCB footprint 'LCSC Part' property (second, stale-looking field) "
             f"disagrees with BOM on {len(pcbpart)} placed refs; uploaded BOM unaffected, but a "
             f"tool reading 'LCSC Part' off the board would pick wrong parts: "
             f"{' '.join(pcbpart[:15])}{' ...' if len(pcbpart) > 15 else ''}")
    else:
        ok(f"{variant}: PCB 'LCSC Part' property agrees with BOM")
    # variant membership
    placed_db = {r for r, d in db.items() if d["cls"] in ("smt_placed", "tht")}
    dnp_db = {r for r, d in db.items() if d["cls"] not in ("smt_placed", "tht")}
    leak = set(bom) & dnp_db
    lost = placed_db - set(bom) - {r for r, f in fps.items() if "exclude_from_bom" in f["attrs"]}
    (fail if leak else ok)(f"{variant}: DNP refs ({fmt_refs(dnp_db)}) leaked into BOM: {fmt_refs(leak)}")
    (fail if lost else ok)(f"{variant}: DB-placed refs missing from BOM: {fmt_refs(lost)}")
    ultra_only = {r for r, g in gen.items() if g["dnp"]}
    plus_only = {r for r, g in gen.items() if g["plus_only"]}
    if variant == "Plus":
        want_in, want_out = plus_only, ultra_only
    else:
        want_in, want_out = ultra_only, plus_only
    bad_in = want_in - set(bom)
    bad_out = want_out & set(bom)
    (fail if bad_in else ok)(f"{variant}: own-variant parts {fmt_refs(want_in)} all in BOM (missing {fmt_refs(bad_in)})")
    (fail if bad_out else ok)(f"{variant}: other-variant parts {fmt_refs(want_out)} absent (present {fmt_refs(bad_out)})")
    # BOM-full consistency
    full = {}
    for row in read_csv(full_path):
        for r in split_refs(row["Designator"]):
            full[r] = row
    fb = [r for r in full if full[r]["Assembly Class"] in ("smt_placed", "tht")]
    (fail if set(fb) != set(bom) else ok)(
        f"{variant}: BOM-full placed set == BOM set (diff {fmt_refs(set(fb) ^ set(bom))}); "
        f"BOM-full non-placed: {fmt_refs(set(full) - set(fb))}")
    nofull = set(fps) - set(full) - {r for r, f in fps.items() if "board_only" in f["attrs"]}
    (fail if nofull else ok)(f"{variant}: every PCB part in BOM-full (missing {fmt_refs(nofull)})")
    return bom


# ------------------------------------------------------------ main
def main():
    skip_g = "--no-gerber" in sys.argv
    out(f"repo {ROOT}")
    gitlog = subprocess.run(["git", "-C", str(ROOT), "log", "-1", "--format=%h %ci",
                             "--", str(PCB)], capture_output=True, text=True).stdout.strip()
    gitfab = subprocess.run(["git", "-C", str(ROOT), "log", "-1", "--format=%h %ci",
                             "--", str(FAB)], capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--",
                            str(CORE / "kicad"), str(CORE / "parts"), str(FAB)],
                           capture_output=True, text=True).stdout.strip()
    out(f"last PCB commit {gitlog}; last fab commit {gitfab}; uncommitted in kicad/parts/fab: "
        f"{dirty or 'none'}")
    fps = pcb_footprints()
    gen = generator_parts()
    dbp = parts_db(CORE / "parts/parts.json")
    dbu = parts_db(CORE / "parts/parts-ultra.json")
    rep = {v: json.loads((CORE / f"reports/{n}").read_text())
           for v, n in (("Plus", "bom_cpl.json"), ("Ultra", "bom_cpl_ultra.json"))}
    out(f"PCB footprints {len(fps)}; attrs: "
        f"{dict(collections.Counter(' '.join(sorted(f['attrs'])) for f in fps.values()))}")
    out(f"PCB dnp-attr refs: {fmt_refs(r for r, f in fps.items() if 'dnp' in f['attrs'])}; "
        f"board_only refs: {fmt_refs(r for r, f in fps.items() if 'board_only' in f['attrs'])}")
    plus_dnp = {r for r, d in dbp.items() if d["cls"] == "dnp"}
    pcb_dnp = {r for r, f in fps.items() if "dnp" in f["attrs"]}
    (fail if plus_dnp != pcb_dnp else ok)(f"PCB dnp attrs == Plus DB dnp (diff {fmt_refs(plus_dnp ^ pcb_dnp)})")
    gen_refs = set(gen)
    pcb_real = {r for r, f in fps.items() if "board_only" not in f["attrs"]}
    (fail if gen_refs != pcb_real else ok)(f"generator refs == PCB refs (gen-only {fmt_refs(gen_refs - pcb_real)}, "
                                           f"pcb-only {fmt_refs(pcb_real - gen_refs)})")
    for v, db in (("Plus", dbp), ("Ultra", dbu)):
        g_bad = [f"{r}(db {db[r]['lcsc']} gen {gen[r]['lcsc']})" for r in sorted(set(db) & set(gen), key=refkey)
                 if db[r]["lcsc"] != gen[r]["lcsc"]]
        (fail if g_bad else ok)(f"{v} parts DB LCSC == generator lcsc= for all {len(set(db) & set(gen))} refs: {' '.join(g_bad)}")
        for k in ("violations",):
            if rep[v].get(k):
                warn(f"{v} reports/bom_cpl*.json {k}: " + "; ".join(x.get("message", str(x)) for x in rep[v][k]))

    check_gerbers(skip_g)

    res = {}
    for v, d, db in (("Plus", FAB, dbp), ("Ultra", FAB / "ultra", dbu)):
        audit = {a["ref"]: a for a in rep[v].get("rotation_audit", [])}
        tbl = sorted(a["ref"] for a in audit.values() if a.get("source") != "lcsc_model")
        if tbl:
            warn(f"{v}: rotation from package table (no LCSC model) for {fmt_refs(tbl)}")
        cpl = check_cpl(fps, v, d / "CPL.csv", db, audit)
        bom = check_bom(fps, v, d / "BOM.csv", d / "BOM-full.csv", db, gen, cpl)
        res[v] = (cpl, bom, d)
    out("\n[2b] raw kicad pos files kept in fab/ (informational)")
    for p in (FAB / "core-pos.csv", FAB / "ultra/core-pos.csv", FAB / "ultra/core-ultra-pos.csv"):
        if p.exists():
            check_pos(fps, p)

    out("\n[3b] new / ECO parts detail (Plus DB; Ultra identical unless noted)")
    focus = ["D6", "D7", "D8", "D9", "U21", "C4", "C8",
             "C228", "C231", "C234", "C237", "C326", "C329", "C332", "C335",
             "R227", "R230", "R233", "R236", "R325", "R328", "R331", "R334",
             "C207", "C208", "C209", "C210", "C307", "C308", "C309", "C310",
             "C222", "C223", "C224", "C225", "C320", "C321", "C322", "C323"]
    for r in focus:
        f, d, du, g = fps.get(r), dbp.get(r), dbu.get(r), gen.get(r)
        inb = {v: (r in res[v][1]) for v in res}
        line = res["Plus"][1].get(r) or res["Ultra"][1].get(r)
        out(f"    {r:5s} pcb {f and f['value']!s:12s} {f and f['layer']!s:5s} {f and f['fpname'][:28]!s:28s} "
            f"BOM[{line and line['Comment']} {line and line['LCSC Part #']}] "
            f"db {d and d['lcsc']} gen {g and g['lcsc']} inPlus={inb['Plus']} inUltra={inb['Ultra']} "
            f"| {d and d['desc'][:70]}")
        if du and d and du["lcsc"] != d["lcsc"]:
            out(f"          Ultra DB lcsc differs: {du['lcsc']}")
    # Zobel rating
    zc = [r for r in ("C228", "C231", "C234", "C237", "C326", "C329", "C332", "C335")]
    rating = {dbp[r]["desc"].split()[1] if r in dbp and len(dbp[r]["desc"].split()) > 1 else None for r in zc}
    comm = {res["Plus"][1][r]["Comment"] for r in zc if r in res["Plus"][1]}
    out(f"    Zobel caps rating from parts.json description: {rating}; BOM Comment {comm} "
        f"(no voltage in BOM Comment column)")
    if rating != {"50V"}:
        fail(f"Zobel caps rating not 50V in parts.json: {rating}")

    out("\n[4] Bottom-side parts")
    bot = sorted((r for r, f in fps.items() if f["layer"] == "B.Cu"), key=refkey)
    for v, (cpl, bom, d) in res.items():
        placed_bot = [r for r in bot if r in cpl]
        wrong = [r for r in placed_bot if cpl[r]["Layer"] != "Bottom"]
        cpl_bot = sorted((r for r in cpl if cpl[r]["Layer"] == "Bottom"), key=refkey)
        (fail if wrong or set(cpl_bot) != set(placed_bot) else ok)(
            f"{v}: {len(placed_bot)} placed parts on B.Cu, all marked Bottom in CPL "
            f"(wrong {fmt_refs(wrong)}): {fmt_refs(placed_bot)}")
        if placed_bot:
            out(f"    {v}: CPL has both Top and Bottom rows -> JLC must assemble both sides "
                f"(Economic PCBA is single-side only, so this needs Standard PCBA or "
                f"the bottom parts hand-fitted)")
    out(f"    all B.Cu footprints incl. DNP/board_only: {fmt_refs(bot)}")
    order = (FAB / "ORDERING.md").read_text()
    m = re.search(r"(\w+) 2-pad MLCCs\s+are on the bottom", order)
    nonmlcc = [r for r in bot if not r.startswith("C")]
    if m and nonmlcc:
        warn(f"ORDERING.md says '{m.group(1)} 2-pad MLCCs are on the bottom ... all non-polar', "
             f"but B.Cu also carries {fmt_refs(nonmlcc)} ({len(bot)} bottom parts total)")

    out("\n[5] Totals")
    for v, (cpl, bom, d) in res.items():
        rows = read_csv(d / "BOM.csv")
        tht = sum(1 for r in cpl if "through_hole" in fps[r]["attrs"])
        out(f"    {v}: CPL placements {len(cpl)} (Top {sum(1 for r in cpl.values() if r['Layer']=='Top')}, "
            f"Bottom {sum(1 for r in cpl.values() if r['Layer']=='Bottom')}, THT {tht}); "
            f"BOM designators {len(bom)}; unique BOM lines {len(rows)}; "
            f"unique LCSC {len({r['LCSC Part #'] for r in rows})}")

    out(f"\nSUMMARY: {len(FAILS)} FAIL, {len(WARNS)} WARN")
    for f in FAILS:
        out(f"  FAIL {f[:300]}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
