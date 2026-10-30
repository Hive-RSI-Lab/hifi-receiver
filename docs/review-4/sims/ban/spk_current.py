#!/usr/bin/env python3
"""spk_current.py  (review-4 BAN; run with $HWDE_PY after `eval "$(/hive/tools/hwde/bin/hwde env)"`)

hwde check_current (IPC-2152) on the speaker copper of Lite after the banana re-route, on a COPY in
/tmp (never on boards/). The board's constraints.json budgets SPK_x at 3.5 A, which the script runs
as DC; here the four SPK nets and the four OUT nets are re-run at:
  2.5 A  = 3.5 A peak sine rms (6 ohm full power / the brief's 3.5 A peak)
  3.2 A  = 4 ohm full-power sine rms at 20 V PVDD with no governor (18.3 Vpk / 4 ohm / sqrt 2)
SPK_x must pass both at dT 30 C; OUT_x is reported only (its 0.31 mm U8 fan-out is R3-LAYOUT-11).
Exits 1 if an SPK net fails.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

SRC = "/work/boards/lite-main/kicad"
skill = os.environ.get("HWDE_SKILL")
if not skill:
    sys.exit("run after: eval \"$(/hive/tools/hwde/bin/hwde env)\"")
tmp = tempfile.mkdtemp(prefix="ban_cur_")
for f in ("lite-main.kicad_pcb", "constraints.json"):
    shutil.copy(os.path.join(SRC, f), tmp)
bad = 0
for amps in (2.5, 3.2):
    c = json.load(open(os.path.join(tmp, "constraints.json")))
    for p in c["power"]:
        if p["net"].startswith(("/SPK_", "/OUT_")):
            p["current_a"] = amps
    cp = os.path.join(tmp, f"c_{amps}.json")
    json.dump(c, open(cp, "w"))
    out = os.path.join(tmp, f"r_{amps}.json")
    subprocess.run([sys.executable, os.path.join(skill, "scripts", "check_current.py"), "--pcb",
                    os.path.join(tmp, "lite-main.kicad_pcb"), "--constraints", cp, "--out", out], check=False)
    r = json.load(open(out))
    viol = {v.get("net") for v in r["violations"]}
    for row in r["checked"]:
        n = row["net"]
        if not n.startswith(("/SPK_", "/OUT_")):
            continue
        need = row["required_mm_by_layer"]["F.Cu"]
        have = min(row["min_track_mm_by_layer"].values())
        ok = n not in viol and have >= need
        tag = ("PASS" if ok else "FAIL") if n.startswith("/SPK_") else ("info" if ok else "INFO-under")
        bad += n.startswith("/SPK_") and not ok
        print(f"{tag} {amps} A {n}: min track {have:.3f} mm, IPC-2152 needs {need:.3f} (+30 C)")
shutil.rmtree(tmp)
sys.exit(1 if bad else 0)
