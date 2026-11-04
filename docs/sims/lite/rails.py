"""Lite-main 5 V / 3.3 V / 3V3A rails: load budget, inductor ripple vs Isat, output ripple with derated caps, LDO.
Values read from boards/lite-main/kicad/lite-main.net and the LCSC records; analytic (not SPICE) - stated per line.
Run:  eval "$(~/modules/bin/hwde env)"; heavy --quick $HWDE_PY docs/sims/lite/rails.py > docs/sims/lite/out_rails.txt
Load currents are ESTIMATES from datasheet-typical values and docs/review-2/thermal.md; nothing measured, nothing run on hardware."""
import os, sys, re, json, math, subprocess
from pathlib import Path
HERE = Path(__file__).resolve().parent; ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "docs/review-5/sims/pa"))
from netlib import Net
N = Net(str(ROOT / "boards/lite-main/kicad/lite-main.net"))
SK = Path(os.environ["HWDE_SKILL"]); PY = os.environ.get("HWDE_PY", sys.executable)
LCSC = ROOT / "boards/lite-main/parts/lcsc"
FAILS = []; THIN = []

def judge(name, val, lim, kind, unit="", note=""):
    ok = val <= lim if kind == "max" else val >= lim
    m = (lim - val) / abs(lim) * 100 if kind == "max" else (val - lim) / abs(lim) * 100
    if not ok: FAILS.append((name, val, lim))
    elif m < 15: THIN.append((name, val, lim, m))
    print(f"  {name:<44s} {val:9.4g} {unit:<3s} limit {'<=' if kind=='max' else '>='} {lim:<8.4g} margin {m:7.1f} %  {'PASS' if ok else 'FAIL'}  {note}")

def num(s):
    m = re.match(r"\s*([\d.]+)\s*([pnuµmkM]?)", s)
    return float(m.group(1)) * {"p": 1e-12, "n": 1e-9, "u": 1e-6, "µ": 1e-6, "m": 1e-3, "": 1, "k": 1e3, "M": 1e6}[m.group(2)]
def desc(ref):
    c = N.comp[ref]; lc = c["fields"].get("LCSC") or c["props"].get("LCSC")
    return json.loads((LCSC / f"{lc}.json").read_text())["results"][0]["description"]
_cache = {}
def ceff(ref, vdc):
    """(typ, low) effective capacitance of an MLCC in the netlist at vdc, hwde mlcc_dcbias generic curve (estimate)."""
    d = desc(ref); rated = int(re.search(r"(\d+)V", d).group(1)); diel = re.search(r"X[57]R|C0G", d).group(0)
    pkg = re.search(r"\b(0402|0603|0805|1206)\b", d).group(1); cs = N.value(ref).replace("F", ""); c = num(cs)
    if diel == "C0G": return c, c
    k = (c, pkg, rated, diel, round(vdc, 2))
    if k not in _cache:
        r = subprocess.run([PY, str(SK / "scripts/mlcc_dcbias.py"), "--c", cs, "--package", pkg, "--rated", str(rated),
                            "--vdc", str(vdc), "--dielectric", diel], capture_output=True, text=True)
        p = json.loads(r.stdout)["part"]; _cache[k] = (p["c_eff"], p["c_eff_low"])
    return _cache[k]

# ---------------- netlist-derived set points ----------------
R9, R10, R11, R12 = (num(N.value(r)) for r in ("R9", "R10", "R11", "R12"))
VFB2, VFB3 = 0.596, 0.600            # TPS54302 / SY8089 reference (datasheet typ, known-unknowns)
V5 = VFB2 * (1 + R9 / R10); V3 = VFB3 * (1 + R11 / R12)
TOL = 1.02 * 1.01                    # assumption: +-2 % reference/regulation, +-1 % feedback resistors
print("== 0. Set points (netlist R9 %.0fk/R10 %.0fk, R11 %.0fk/R12 %.0fk)" % (R9/1e3, R10/1e3, R11/1e3, R12/1e3))
print(f"  +5V  = {V5:.3f} V nominal, {V5/TOL:.3f}..{V5*TOL:.3f} V with +-2 % ref + 1 % resistors (assumed tolerances)")
print(f"  +3V3 = {V3:.3f} V nominal, {V3/TOL:.3f}..{V3*TOL:.3f} V")
judge("+5V max vs SY8089 Vin recommended 5.5 V", V5 * TOL, 5.5, "max", "V", "(5.5 V recalled from datasheet, verify; abs max 6 V)")
judge("+5V max vs SY8089 Vin abs max 6 V", V5 * TOL, 6.0, "max", "V")
judge("+3V3 min vs ESP32 min 3.0 V", V3 / TOL, 3.0, "min", "V")
judge("+3V3 max vs ES8388/ESP32 max 3.6 V", V3 * TOL, 3.6, "max", "V")

# ---------------- load budget ----------------
print("\n== 1. Load budget (ESTIMATES: datasheet-typical, review-2 thermal.md; not measured)")
loads3 = [  # name, typ mA, max mA, note
    ("U5 ESP32-WROVER-E-N8R8 (Wi-Fi stream/BT; max = TX peak, Espressif asks 500 mA)", 120, 500, "typ per review-2 thermal.md"),
    ("U6 CH340B", 10, 20, "estimate"),
    ("U7 ES8388 digital (pins 2,3)", 8, 15, "estimate"),
    ("pull-ups R4,R20-R23,R53,R60-R63 (typ 25 % low, max all low)", 1.5, 5.4, "3.3/10k=0.33 mA, 3.3/2.2k=1.5 mA"),
    ("IR receiver U9 via R65 + LED D2 via R64 1k", 2.9, 3.0, "estimate (IRM-3638T ~1.5 mA, red LED 1.4 mA)"),
    ("FB divider R11/R12", 0.02, 0.02, "3.3/183k"),
]
i3t = sum(l[1] for l in loads3) / 1e3; i3m = sum(l[2] for l in loads3) / 1e3
for l in loads3: print(f"  +3V3  {l[0]:<80s} typ {l[1]:6.2f} mA  max {l[2]:6.1f} mA   ({l[3]})")
i4t, i4m = 0.025, 0.040
print(f"  +3V3A U7 ES8388 analog via U4 LP5907                                              typ {i4t*1e3:6.2f} mA  max {i4m*1e3:6.1f} mA   (review-2 ~25 mA unverified; max estimate)")
print(f"  => +3V3 total  typ {i3t*1e3:.0f} mA, max (all peaks coincident) {i3m*1e3:.0f} mA;  +3V3A typ {i4t*1e3:.0f} mA / max {i4m*1e3:.0f} mA")
EFF3 = 0.90; EFF5 = 0.85          # estimates at these loads
i5t = V3 * i3t / (V5 * EFF3) + i4t + 0.0001; i5m = V3 * i3m / (V5 * EFF3) + i4m + 0.0001
print(f"  +5V  = SY8089 input (eta {EFF3:.2f}) + U4 + R9/R10/R80 ~0.1 mA:  typ {i5t*1e3:.0f} mA, max {i5m*1e3:.0f} mA   (review-2 used 0.2 A avg / 0.4 A peak)")
v20 = 20.0
i20t = V5 * i5t / (EFF5 * v20); i20m = V5 * i5m / (EFF5 * v20)
print(f"  VBUS draw for the rails at 20 V (U2 eta {EFF5}): typ {i20t*1e3:.0f} mA ({v20*i20t:.2f} W), max {i20m*1e3:.0f} mA ({v20*i20m:.2f} W); plus CH224A, divider (est.)")
judge("U2 TPS54302 max load vs 3 A rating", i5m, 3.0, "max", "A")
judge("U3 SY8089 max load vs 2 A rating", i3m, 2.0, "max", "A")
judge("U4 LP5907 max load vs 250 mA", i4m, 0.25, "max", "A")

# ---------------- inductors ----------------
print("\n== 2. Inductor ripple and peak (L = nominal -20 %/-30 % tolerance is the worst case, fsw -20 %)")
def buck(tag, vin, vout, L, f, iout, isat, irms, lname, tolL, ftol=0.8):
    d = vout / vin
    dil = vout * (1 - d) / (L * tolL * f * ftol)
    ipk = iout + dil / 2
    irm = math.sqrt(iout ** 2 + dil ** 2 / 12)
    print(f" -- {tag}: Vin {vin} V Vout {vout:.2f} V D {d:.3f}, L {L*1e6:.1f} uH x{tolL}, fsw {f/1e3:.0f} kHz x{ftol}: dIL {dil:.3f} A pp")
    judge(f"{lname} Ipk (Iout {iout*1e3:.0f} mA) vs Isat", ipk, isat, "max", "A")
    judge(f"{lname} Irms vs rated", irm, irms, "max", "A")
    return dil
# L5 SWPA6045S100MT: Isat 3.2 A / Irms 2.45 A (datasheet per known-unknowns power:22; LCSC lists 3.5 A)
for vin in (15.0, 20.0, 21.0, 22.0):
    for tag, io in (("typ load", i5t), ("max load", i5m)):
        buck(f"U2/L5 {tag}", vin, V5, 10e-6, 400e3, io, 3.2, 2.45, "L5", 0.8)
# also the TPS54302 high-side current limit (datasheet min ~3.2 A, recalled): peak must stay well below
for vin in (5.0, 4.5):
    for tag, io in (("typ load", i3t), ("max load", i3m)):
        buck(f"U3/L6 {tag}", vin, V3, 2.2e-6, 1.5e6, io, 3.4, 1.85, "L6", 0.7)

# ---------------- output ripple ----------------
print("\n== 3. Output ripple, derated caps (cap term dI/(8 f C) + 10 mOhm total ESR/ESL allowance; PFM/Eco light-load ripple NOT covered)")
def ripple(tag, refs, v, dil, f):
    out = {}
    for ci, corner in enumerate(("typ", "low")):
        c = sum(ceff(r, v)[ci] for r in refs)
        vr = dil / (8 * f * c) + dil * 0.010
        out[corner] = (c, vr)
    print(f" -- {tag}: caps {refs} at {v:.2f} V: Ceff typ {out['typ'][0]*1e6:.1f} uF, low {out['low'][0]*1e6:.1f} uF (nominal {sum(num(N.value(r).replace('F','')) for r in refs)*1e6:.1f} uF)")
    return out
d5 = buck("U2 worst-case for ripple: Vin 22 V, max load", 22.0, V5, 10e-6, 400e3, i5m, 3.2, 2.45, "L5 (ripple case)", 0.8)
o5 = ripple("+5V", ["C7", "C8", "C25", "C10", "C11", "C15"], V5, d5, 400e3 * 0.8)
for k in ("typ", "low"): judge(f"+5V ripple pp, caps {k} (target 50 mV = 1 %, assumed)", o5[k][1] * 1e3, 50.0, "max", "mV")
d3 = buck("U3 worst-case for ripple: Vin 4.5 V... 5.4 V, max load", 5.4, V3, 2.2e-6, 1.5e6, i3m, 3.4, 1.85, "L6 (ripple case)", 0.7)
o3 = ripple("+3V3", ["C12", "C13", "C20", "C21", "C23", "C24", "C30", "C31"], V3, d3, 1.5e6 * 0.8)
for k in ("typ", "low"): judge(f"+3V3 ripple pp, caps {k} (target 50 mV, assumed)", o3[k][1] * 1e3, 50.0, "max", "mV")
# input cap rms for U2 on VBUS
iin_rms = i5m * math.sqrt((V5 / 20) * (1 - V5 / 20))
cin = sum(ceff(r, 20.0)[1] for r in ("C1", "C3", "C4", "C5"))
print(f" -- U2 input: Icin_rms {iin_rms*1e3:.0f} mA at max load (VBUS ceramics {cin*1e6:.1f} uF derated low; dVin pp ~ {i5m*(V5/20)*(1-V5/20)/(400e3*0.8*cin)*1e3:.0f} mV)")

# ---------------- LDO ----------------
print("\n== 4. U4 LP5907-3.3 (+3V3A): dropout and dissipation (Iq 12 uA from LCSC; dropout 250 mV at 250 mA is the datasheet worst case, used for all loads)")
vin_lo, vin_hi = V5 / TOL, V5 * TOL
for tag, io in (("typ", i4t), ("max", i4m)):
    judge(f"U4 headroom Vin_min-3.3 V x1.03 ({tag})", vin_lo - 3.3 * 1.03, 0.25, "min", "V")
    pd = (vin_hi - 3.3) * io + vin_hi * 12e-6
    print(f"  U4 dissipation {tag}: ({vin_hi:.2f}-3.3) x {io*1e3:.0f} mA + Iq = {pd*1e3:.0f} mW, rise at 193.4 C/W (TI SNVS798 DBV, via review-2) = {pd*193.4:.1f} K")
    judge(f"U4 Tj at 52 C box air ({tag}), 125 C limit", 52 + pd * 193.4, 125.0, "max", "C")

print("\n== SUMMARY")
print("FAILS:", [f"{n}: {v:.4g} vs {l:g}" for n, v, l in FAILS] or "none")
print("THIN (<15 %):", [f"{n}: {v:.4g} vs {l:g} ({m:.1f} %)" for n, v, l, m in THIN] or "none")
sys.exit(1 if FAILS else 0)
