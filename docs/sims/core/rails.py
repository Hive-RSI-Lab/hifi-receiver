"""core low-voltage rails: U4 SY8303 (VBUS -> +5V, L9 4.7 uH), U5 SY8089 (+5V -> +3V3,
L10 2.2 uH), U6 LP5907-3.3 (+5V -> +3V3A). Values read from core.net (asserted below);
output caps derated with hwde mlcc_dcbias. Hand calc, not SPICE. Load currents are
datasheet-typical ESTIMATES (marked est), not measurements: the INA186 reads the real
total at bring-up. The ultra-front load over J3 comes from docs/sims/uf (UF_5V/UF_3V3).
Checks: load vs regulator rating, inductor Ipk vs Isat, output ripple vs 1 % of Vout,
regulator Tj at the governed box air (thermal.py). Exit 1 on fail."""
import json, os, subprocess, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "docs/review-5/sims/pa"))
from netlib import Net
N = Net(str(ROOT / "boards/core/kicad/core.net"))
SK = os.environ["HWDE_SKILL"]

def ceff(c, pkg, rated, v):
    o = subprocess.run([sys.executable, f"{SK}/scripts/mlcc_dcbias.py", "--c", c, "--package", pkg,
                        "--rated", str(rated), "--vdc", str(v), "--dielectric", "X5R"], capture_output=True, text=True).stdout
    p = json.loads(o)["part"]; return p["c_eff"], p["c_eff_low"]

bad = []
def need(cond, msg):
    if not cond: bad.append(msg)
# netlist facts this calc uses
for ref, nets, val in (("R13", ["+5V", "BUCK5_FB"], "15k"), ("R14", ["BUCK5_FB", "GND"], "2k"),
                       ("R15", ["+3V3", "BUCK3_FB"], "100k"), ("R16", ["BUCK3_FB", "GND"], "22k"),
                       ("R12", ["BUCK5_FS", "GND"], "100k"), ("L9", ["+5V", "BUCK5_LX"], "4.7uH"),
                       ("L10", ["+3V3", "BUCK3_LX"], "2.2uH")):
    need(sorted(N.pins_of(ref).values()) == sorted(nets) and N.value(ref) == val, f"{ref} changed")
for ref in ("C13", "C14", "C16", "C17", "C30"): need(N.comp[ref]["fields"].get("LCSC") == "C45783", f"{ref} part")
need(N.comp["C15"]["fields"].get("LCSC") == "C19702", "C15 part")

V5 = 0.6 * (1 + 15 / 2); V3 = 0.6 * (1 + 100 / 22); VIN = 28.0
F5 = 105e3 * 1e3 / 100e3 * 1e3   # SY8303 fs = 105/R(k) MHz -> 1.05 MHz (AN_SY8303)
F3 = 1.5e6                         # SY8089 fixed 1.5 MHz (datasheet typ)
UF_5V = float(os.environ.get("UF_5V", 0.25)); UF_3V3 = float(os.environ.get("UF_3V3", 0.05))
# +3V3A loads (est): ES9018K2M AVCC + PCM5102A AVDD + ES7243E AVDD
I3A = 0.050
# +3V3 loads (est, avg / peak): ESP32-WROVER-E WiFi 0.24 / 0.50, LAN8720A 0.055, ES9018K2M DVCC 0.025,
# CH340B 0.012, PCM5102A+ES7243E digital 0.008, Y1+Y2 oscillators 0.025, pull-ups/LED 0.005
I3 = 0.24 + 0.055 + 0.025 + 0.012 + 0.008 + 0.025 + 0.005 + UF_3V3
I3pk = I3 + 0.26
# +5V loads: 2 x TPA3220 GVDD+AVDD 0.026 each (est from TPA3221 DS), +5VA (OPA1678, switches, R90) 0.01,
# SY8089 input, LP5907 input, ultra-front over J3
def i5(i3): return 2 * 0.026 + 0.010 + I3A + V3 * i3 / (0.90 * V5) + UF_5V
I5, I5pk = i5(I3), i5(I3pk)
print(f"+5V  = {V5:.3f} V  (0.6 x (1 + R13/R14))     +3V3 = {V3:.3f} V (0.6 x (1 + R15/R16))")
print(f"load budget (est): +3V3 {I3*1e3:.0f} mA avg / {I3pk*1e3:.0f} mA WiFi peak (ultra-front {UF_3V3*1e3:.0f} mA); "
      f"+3V3A {I3A*1e3:.0f} mA; +5V {I5*1e3:.0f} / {I5pk*1e3:.0f} mA (ultra-front {UF_5V*1e3:.0f} mA)")
need(I5pk < 3.0, "SY8303 over 3 A"); need(I3pk < 2.0, "SY8089 over 2 A"); need(I3A < 0.25, "LP5907 over 250 mA")
print(f"  ratings: SY8303 3 A -> {100*I5pk/3:.0f} % at peak; SY8089 2 A -> {100*I3pk/2:.0f} %; LP5907 250 mA -> {100*I3A/0.25:.0f} %")

D5 = V5 / VIN; dI5 = V5 * (1 - D5) / (4.7e-6 * F5)
D3 = V3 / V5;  dI3 = V3 * (1 - D3) / (2.2e-6 * F3)
ipk5, ipk3 = I5pk + dI5 / 2, I3pk + dI3 / 2
print(f"SY8303 at {VIN:.0f} V: D {D5:.2f}, ripple {dI5:.2f} A p-p, Ipk {ipk5:.2f} A vs L9 Isat 2.9 A (SWPA4030S4R7MT) "
      f"-> margin {100*(1-ipk5/2.9):.0f} %")
print(f"SY8089: D {D3:.2f}, ripple {dI3:.2f} A p-p, Ipk {ipk3:.2f} A vs SY8089 2 A rating (L10 SWPA4030S2R2MT Isat not on file)")
need(ipk5 < 2.9, "L9 saturates")
c22_5 = ceff("22u", "0805", 25, V5); c10_5 = ceff("10u", "0603", 10, V5); c22_3 = ceff("22u", "0805", 25, V3)
co5 = 2 * c22_5[1] + c10_5[1]; co3 = 3 * c22_3[1]
r5 = dI5 / (8 * F5 * co5) + dI5 * 0.003; r3 = dI3 / (8 * F3 * co3) + dI3 * 0.003   # + 3 mohm ESR (est)
print(f"+5V out caps C13+C14+C15 at {V5:.1f} V (low corner): {co5*1e6:.1f} uF -> ripple {r5*1e3:.1f} mV p-p vs {V5*10:.0f} mV")
print(f"+3V3 out caps C16+C17+C30 at {V3:.1f} V (low corner): {co3*1e6:.1f} uF -> ripple {r3*1e3:.1f} mV p-p vs {V3*10:.0f} mV")
need(r5 < 0.01 * V5 and r3 < 0.01 * V3, "ripple")
# dissipation (review-2 thermal.md method), Tj at the governed box air from thermal.py
AIR = float(os.environ.get("BOX_AIR_C", 55.3))
p4 = I5**2 * (0.11 * D5 + 0.07 * (1 - D5)) + 0.5 * VIN * I5 * 20e-9 * F5 + 0.5 * 200e-12 * VIN**2 * F5 + 0.03
p5 = V3 * I3 * (1 / 0.90 - 1); p6 = (V5 - V3) * I3A
for name, p, rth, tmax in (("U4 SY8303", p4, 80, 150), ("U5 SY8089", p5, 120, 125), ("U6 LP5907", p6, 193.4, 125)):
    tj = AIR + p * rth
    print(f"{name}: {p:.2f} W x {rth} C/W + box air {AIR:.1f} C = Tj {tj:.0f} C vs {tmax} C, margin {tmax-tj:.0f} K (est)")
    need(tj < tmax, f"{name} Tj")
print("FAIL: " + "; ".join(bad) if bad else "ALL PASS")
sys.exit(1 if bad else 0)
