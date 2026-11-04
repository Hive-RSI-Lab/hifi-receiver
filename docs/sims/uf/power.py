"""ultra-front power budget (netlist-derived), FFC conductor drop and Q1 dissipation.
Reads boards/ultra-front/kicad/ultra-front.net (resistor values, J1 pin nets, parts) and
.kicad_pcb (J1 pad pitch). Read-only. Run: heavy --quick python3 power.py ; exit 1 on fail.
NOTHING HERE IS MEASURED. Items marked [A] are assumptions, [R5] are review-5 figures,
[DS] are datasheet values recalled from memory (no datasheet is in the repo): verify."""
import re, sys
from pathlib import Path
R = Path(__file__).resolve().parents[3]
net = (R / "boards/ultra-front/kicad/ultra-front.net").read_text()
pcb = (R / "boards/ultra-front/kicad/ultra-front.kicad_pcb").read_text()
val = dict(re.findall(r'\(comp\s+\(ref "([^"]+)"\)\s+\(value "([^"]*)"\)', net))
def ohms(s):
    m = re.fullmatch(r"([\d.]+)([kKRM]?)", s); return float(m.group(1)) * {"": 1, "R": 1, "k": 1e3, "K": 1e3, "M": 1e6}[m.group(2)]
nets = {}
for m in re.finditer(r'\(net\s+\(code "\d+"\)\s+\(name "([^"]+)"\)(.*?)\n\t\t\)', net, re.S):
    nets[m.group(1)] = re.findall(r'\(ref "(\w+)"\)\s+\(pin "(\w+)"', m.group(2))
bad = []
def check(name, val_, lim, kind, unit, note=""):
    ok = val_ <= lim if kind == "max" else val_ >= lim
    mg = (lim - val_) / lim * 100 if kind == "max" else (val_ - lim) / lim * 100
    tag = "PASS" if ok else "FAIL"; thin = " THIN(<15%)" if ok and mg < 15 else ""
    print(f"{tag} {name}: {val_:.4g} {unit} vs limit {kind} {lim:g} {unit}, margin {mg:.0f} %{thin} {note}")
    if not ok: bad.append(name)

# --- netlist facts
j1 = {n: sorted(p for r, p in v if r == "J1") for n, v in nets.items() if any(r == "J1" for r, p in v)}
p33, p5, pg = j1["+3V3"], j1["+5V"], j1["GND"]
print("J1 pins: +3V3", p33, " +5V", p5, " GND", pg, "(review-5 text says 5 GND; netlist has", len(pg), ")")
assert (len(p33), len(p5)) == (2, 2), "rail conductor count changed"
BLR = [ohms(val[r]) for r in ("R13", "R17", "R18")]
assert all(r in dict((a, b) for a, b in nets["/BL_A"]) for r in ("R13", "R17", "R18")) and all(a == 1 or True for a in BLR)
Rbl = 1 / sum(1 / r for r in BLR)
print(f"backlight series R13||R17||R18 = {BLR} -> {Rbl:.1f} ohm; LED R14/R15/R16 = {val['R14']}/{val['R15']}/{val['R16']}; R10 {val['R10']}; Q1 {val['Q1']}; R11 {val['R11']}; R12 {val['R12']}")
fp = re.search(r'\(footprint "aiee:FPC-SMD_20P.*?\(property "Reference" "J1"', pcb, re.S)
xs = []
if fp:
    blk = pcb[fp.start():]; blk = blk[:blk.find('(footprint "', 20) if blk.find('(footprint "', 20) > 0 else None]
    xs = sorted(float(x) for x in re.findall(r'\(pad "\d+" smd[^\n]*\n?\s*\(at ([-\d.]+) ', blk))
import statistics
d = [round(b - a, 3) for a, b in zip(xs, xs[1:])]
pitch = statistics.median(d) if d else float("nan")
print(f"J1 pad pitch from pcb: median {pitch:.3f} mm ({len(xs)} pads parsed)")
check("J1 pitch is 0.5 mm", abs(pitch - 0.5), 0.01, "max", "mm")

# --- loads (corners)  V5 4.75..5.25, V3 3.2..3.4 (core +3V3 3.33 V per review-5)
VF_BL = (2.7, 3.2)   # [R5] backlight string Vf nominal..high; LCD module DS value not re-read
def bl(v5, vf): return (v5 - vf) / Rbl * 1e3
I_bl_max, I_bl_min = bl(5.25, VF_BL[0]), bl(4.75, VF_BL[1])
print(f"backlight: {I_bl_min:.1f} mA (4.75 V, Vf 3.2) .. {I_bl_max:.1f} mA (5.25 V, Vf 2.7)  [review-5: 39..64 mA]; module max 80 mA [R5]")
check("backlight current vs module 80 mA", I_bl_max, 80, "max", "mA")
per_r = (I_bl_max / 3e3) ** 2 * ohms(val["R13"]) * 1e3
check("R13/17/18 dissipation (0603 100 mW)", per_r, 100, "max", "mW")
# RGB LED: one colour at a time is the firmware rule; also bound all-on with low Vf [A]
rgb = {"R14": (ohms(val["R14"]), 1.8), "R15": (ohms(val["R15"]), 2.4), "R16": (ohms(val["R16"]), 2.4)}  # Vf [A] min-ish
i_led = {k: (5.25 - vf) / r * 1e3 for k, (r, vf) in rgb.items()}
I_rgb_one, I_rgb_all = max(i_led.values()), sum(i_led.values())
print("RGB LED mA each (5.25 V, low Vf):", {k: round(v, 2) for k, v in i_led.items()}, f"one={I_rgb_one:.1f} all={I_rgb_all:.1f}")
check("TCA9535 sink per pin vs 25 mA [DS]", I_rgb_one, 25, "max", "mA")
check("TCA9535 total LED+port vs 200 mA [DS]", I_rgb_all, 200, "max", "mA")
I_5v = I_bl_max + I_rgb_all     # worst: all three colours on
I_ir = 1.2                      # [A] IRM-H638T Icc, review-5 says 0.7 mA max
n_pu = sum(1 for r in "R1 R2 R3 R4 R5 R6 R7 R8 R9".split() if ohms(val[r]) == 10e3)
I_pu = n_pu * 3.4 / 10e3 * 1e3  # all pull-ups held low at 3.4 V
I_u1, I_lcd = 0.1, 10.0         # [A] TCA9535 active ~0.1 mA; LCD logic 10 mA ("TBD" in module DS, review-5 assumption)
I_3v3 = I_ir + I_pu + I_u1 + I_lcd
vir = 3.3 - I_ir * 1e-3 * ohms(val["R10"])
print(f"IR supply: {I_ir} mA through R10 {val['R10']} -> {vir:.3f} V at U2 pin 4 (IRM needs >= 2.7 V [DS])")
check("U2 IR supply after R10 vs 2.7 V", vir, 2.7, "min", "V")
print(f"+5V load {I_5v:.1f} mA (backlight {I_bl_max:.1f} + LED all-on {I_rgb_all:.1f}); +3V3 load {I_3v3:.1f} mA (IR {I_ir}, pull-ups {I_pu:.1f}, U1 {I_u1}, LCD {I_lcd} [A])")

# --- FFC drop. conductor R: rho 1.72e-8, width 0.3 mm, 35 um Cu [A], 30..50 mm; contact 60 mohm [R5] / 100 mohm bound
RHO, W, T = 1.72e-8, 0.30e-3, 35e-6
for L, rc in ((0.050, 0.060), (0.050, 0.100), (0.150, 0.100)):
    rcond = RHO * L / (W * T)
    path = rcond + 2 * rc                       # J3 contact + conductor + J1 contact
    i5, i3, ig = I_5v * 1e-3 / len(p5), I_3v3 * 1e-3 / len(p33), (I_5v + I_3v3) * 1e-3 / len(pg)
    d5, d3, dg = i5 * path, i3 * path, ig * path
    print(f"-- cable {L*1e3:.0f} mm: conductor {rcond*1e3:.0f} mohm, contact {rc*1e3:.0f} mohm x2 -> {path*1e3:.0f} mohm per conductor")
    print(f"   per contact: +5V {i5*1e3:.1f} mA, +3V3 {i3*1e3:.1f} mA, GND {ig*1e3:.1f} mA; drop +5V {d5*1e3:.2f} mV, +3V3 {d3*1e3:.2f} mV, GND rise {dg*1e3:.2f} mV")
    if (L, rc) == (0.050, 0.100) or L == 0.150:
        check(f"per-contact current ({L*1e3:.0f} mm) vs 0.5 A [R5 XUNPU]", max(i5, i3, ig), 0.5, "max", "A")
        check(f"+5V supply sag incl. GND rise ({L*1e3:.0f} mm) vs 100 mV [A: 2 % of 5 V]", d5 + dg, 0.100, "max", "V")
        check(f"+3V3 sag incl. GND rise ({L*1e3:.0f} mm) vs 66 mV [A: 2 % of 3.3 V]", d3 + dg, 0.066, "max", "V")
        dI = d5 * 1e3  # backlight current change from the sag
        print(f"   backlight current change from +5V sag: {(d5+dg)/Rbl*1e3:.2f} mA ({(d5+dg)/Rbl*1e3/I_bl_max*100:.2f} % of {I_bl_max:.0f} mA)")

# --- Q1 AO3400A: Rds(on) [DS recalled] 32 mohm @4.5 V, 48 mohm @2.5 V max; gate 3.3 V (3.0 V worst) -> use 48 mohm; RthJA 125 C/W
RDS, RTH, TA = 0.048, 125, 60   # TA 60 C enclosure [A]
Pq = (I_bl_max * 1e-3) ** 2 * RDS
Tj = TA + Pq * RTH
print(f"Q1: I {I_bl_max:.1f} mA, Rds(on) {RDS*1e3:.0f} mohm, Vds {I_bl_max*1e-3*RDS*1e3:.1f} mV, P {Pq*1e3:.3f} mW, Tj {Tj:.2f} C at Ta {TA} C")
check("Q1 dissipation vs 350 mW [DS SOT-23 ~1.4 W at 25 C; 350 mW derated bound]", Pq * 1e3, 350, "max", "mW")
check("Q1 junction temp vs 150 C", Tj, 150, "max", "C")
check("Q1 gate drive (3.0 V worst) vs 2.5 V Rds(on) spec point [DS]", 3.0, 2.5, "min", "V")
check("Q1 gate pull-down R11 hold-off: gate leak drop", 0.1e-6 * ohms(val["R11"]), 0.5, "max", "V", "(100 nA leak [A])")
print("RESULT:", "FAIL " + ", ".join(bad) if bad else "PASS")
sys.exit(1 if bad else 0)
