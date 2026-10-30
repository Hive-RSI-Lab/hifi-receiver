"""Writes the BTL output-filter testbenches (one per board x load x corner).
Each leg: L (with DCR) from the half-bridge (ideal source, +/-0.5 V) to the
speaker node, C (with ESR) to GND, optional snubber R+C to GND; RL across
the two speaker nodes. Measures |H| = V(spk+ - spk-)/V(drive) in dB."""
import json
from pathlib import Path

CASES = {
    # board: L, DCR, C nominal, snubber (R, C) or None
    "lite": dict(L=10e-6, dcr=0.030, C=1.0e-6, sn=None),
    "core": dict(L=10e-6, dcr=0.030, C=1.0e-6, sn=(4.7, 1e-9)),
}
CORNERS = {
    # name: (L factor, C factor)  - C factor = DC-bias derating at PVDD/2
    "nom": (1.0, 1.0),
    "derated": None,  # filled per board below
}
DERATE = {"lite": (0.8, 0.85), "core": (0.8, 0.60)}   # L -20 % tol, C at 10 V (1206) / 14 V (0805)
LOADS = {"4": 4.0, "6": 6.0, "8": 8.0, "open": 1e6}

for board, p in CASES.items():
    for corner in ("nom", "derated"):
        lf, cf = (1.0, 1.0) if corner == "nom" else DERATE[board]
        for lname, rl in LOADS.items():
            name = f"lc_{board}_{corner}_{lname}"
            sn = ""
            if p["sn"]:
                r, c = p["sn"]
                sn = (f"Rsa a sa {r}\nCsa sa 0 {c}\nRsb b sb {r}\nCsb sb 0 {c}\n")
            cir = f"""* {name}: BTL LC filter, L={p['L']*lf:.3g} DCR={p['dcr']} C={p['C']*cf:.3g} per leg, RL={rl}
Vp dp 0 DC 0 AC 0.5
Vn dn 0 DC 0 AC 0.5 180
La dp la {p['L']*lf}
Rla la a {p['dcr']}
Lb dn lb {p['L']*lf}
Rlb lb b {p['dcr']}
Ca a ca {p['C']*cf}
Rca ca 0 0.005
Cb b cb {p['C']*cf}
Rcb cb 0 0.005
{sn}RL a b {rl}
Ed d 0 a b 1
.ac dec 400 100 2Meg
.measure ac hpk MAX vdb(d)
.measure ac f0 WHEN vp(d)=-1.5708
.measure ac h1k FIND vdb(d) AT=1k
.measure ac h20k FIND vdb(d) AT=20k
.measure ac f3 WHEN vdb(d)=-3 FALL=LAST
.measure ac h400k FIND vdb(d) AT=400k
.measure ac h600k FIND vdb(d) AT=600k
.end
"""
            Path(f"{name}.cir").write_text(cir)
            bounds = [
                {"measure": "h20k", "min": -3.0, "max": 3.0, "severity": "warning",
                 "msg": "audio band edge within +/-3 dB"},
                {"measure": "hpk", "max": 6.0 if lname != "open" else 40.0,
                 "severity": "warning", "msg": "resonant peaking"},
            ]
            Path(f"{name}.bounds.json").write_text(json.dumps(bounds, indent=1))
print("ok")
