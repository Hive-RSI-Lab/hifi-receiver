"""Class-D output filter stability, core TPA3220 (U15/U16, 4 BTL outputs per board) and
lite TPA3118D2 (U8): each leg's LC (+ Zobel on core) into open, 4, 8 ohm and two reactive
speaker models. Values are read from the current netlists and asserted. Linear AC,
closed form (complex impedances, exact for this ladder), 10 Hz-500 kHz.
Both amps take feedback before the filter, so the LC sits outside the loop: the risks
are the filter's own resonance (peaking into a light load, ringing on ultrasonic content)
and a low impedance the chip has to drive. Filter caps are DC-biased at PVDD/2 (BD mode
common mode): mlcc_dcbias c_eff_low used as the worst (low C, higher f0) corner, plus
nominal. Limits:
  * resistive 4 and 8 ohm: 20 Hz-20 kHz flat to +/-1.5 dB, and min |Z| at the chip
    (BTL, 20 Hz-20 kHz) >= 3.2 ohm (4 ohm BTL rating of both chips, -20 %)
  * open load and the two speaker models (their Le makes them look open above ~10 kHz):
    peak <= +22 dB on core (core-zobel.md acceptance, derated 470 nF case); lite has no
    Zobel, the accepted risk in core-zobel.md, so its peak is reported, not passed
  * speaker models: the filter may not pull |Z| at the chip more than 5 % under the
    speaker's own minimum (the 2-way model itself dips to ~3.1 ohm, as real ones do)
Exit 1 on a fail."""
import cmath, math, os, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "docs/review-5/sims/pa"))
import netlib

def chk(n, ref, val, lcsc=None):
    v = n.value(ref); assert v.split()[0] == val, (ref, v, val)
    if lcsc: assert n.comp[ref]["fields"].get("LCSC") == lcsc, (ref, n.comp[ref]["fields"].get("LCSC"))
core = netlib.Net(str(ROOT / "boards/core/kicad/core.net"))
for L in ("L1", "L2", "L3", "L4"): chk(core, L, "10uH", "C2962885")
for C in ("C226", "C229", "C232", "C235"): chk(core, C, "1uF", "C28323")
for R in ("R227", "R230", "R233", "R236"): chk(core, R, "4.7R", "C23164")
for C in ("C228", "C231", "C234", "C237"): chk(core, C, "470nF", "C513577")
assert core.pins_of("R227") == {"1": "A_SPK_L-", "2": "A_OUT1M_SN"} and core.pins_of("C228") == {"1": "A_OUT1M_SN", "2": "GND"}
lite = netlib.Net(str(ROOT / "boards/lite-main/kicad/lite-main.net"))
for L in ("L1", "L2", "L3", "L4"): chk(lite, L, "10uH", "C48391614")
for C in ("C68", "C69", "C70", "C71"): chk(lite, C, "1uF", "C1848")
for net in ("SPK_L_P", "SPK_L_N", "SPK_R_P", "SPK_R_N"):
    assert not [r for r in lite.refs_on(net) if r[0] == "R" and lite.value(r) in ("4.7R", "3.3R")], "lite Zobel appeared"
print("netlist values asserted: core L 10uH C2962885, C 1uF/50V 0805 C28323, Zobel 4.7R + 470nF/50V 0603 C513577 per leg;"
      " lite L 10uH C48391614, C 1uF/50V 1206 C1848, no Zobel")

# mlcc_dcbias (hwde) at PVDD/2: core 28 V -> 14 V, lite 20 V -> 10 V, X7R
BOARDS = {
  "core TPA3220": dict(L=10e-6, dcr=0.030, C={"nominal": 1e-6, "14 V bias (c_eff_low)": 0.781e-6},
                       zob=(4.7, {"nominal": 470e-9, "14 V bias (c_eff_low)": 0.335e-6}), pk_lim=22.0),
  "lite TPA3118": dict(L=10e-6, dcr=0.030, C={"nominal": 1e-6, "10 V bias (c_eff_low)": 0.948e-6}, zob=None, pk_lim=None),
}
# speakers (differential); per leg = half of Z to the virtual ground of the BTL pair
def woofer(w, Re=3.2, Le=0.4e-3, fs=45, Qes=0.4, Qms=4.0):      # 4 ohm woofer, Thiele-Small electrical analog
    Cm = Qes / (2 * math.pi * fs * Re); Lm = 1 / ((2 * math.pi * fs) ** 2 * Cm); Rm = Re * Qms / Qes
    zm = 1 / (1 / Rm + 1 / (1j * w * Lm) + 1j * w * Cm)
    return Re + 1j * w * Le + zm
def twoway(w):                                                    # 8 ohm 2-way, 2nd-order crossover at 2.5 kHz
    wo = woofer(w, Re=6.0, Le=0.8e-3, fs=50); tw = 6.0 + 1j * w * 0.05e-3
    lp = 1j * w * 0.72e-3 + 1 / (1 / wo + 1j * w * 5.6e-6)           # series L, shunt C
    hp = 1 / (1j * w * 5.6e-6) + 1 / (1 / tw + 1 / (1j * w * 0.72e-3))  # series C, shunt L
    return 1 / (1 / lp + 1 / hp)
LOADS = {"open": lambda w: 1e9, "4 ohm": lambda w: 4.0, "8 ohm": lambda w: 8.0,
         "4 ohm woofer (Re 3.2, Le 0.4 mH, fs 45)": woofer, "8 ohm 2-way + crossover": twoway}

def resp(b, cf, zc, load, f):
    w = 2 * math.pi * f
    zl = load(w) / 2
    zsh = 1 / (1j * w * cf)
    if b["zob"]: zsh = 1 / (1 / zsh + 1 / (b["zob"][0] + 1 / (1j * w * zc)))
    zsh_l = 1 / (1 / zsh + 1 / zl)
    zin = 1j * w * b["L"] + b["dcr"] + zsh_l
    return zsh_l / zin, 2 * zin                                   # leg transfer, BTL impedance at the chip
F = [10 * 10 ** (i / 200) for i in range(0, 1000)]               # 10 Hz .. 1 MHz, 200 pts/dec
fails, rows = [], []
for name, b in BOARDS.items():
    print(f"\n== {name}: L {b['L']*1e6:.0f} uH / DCR {b['dcr']*1e3:.0f} mohm per leg" + (f", Zobel {b['zob'][0]} R + C" if b["zob"] else ", no Zobel"))
    print(f"{'cap corner':24s} {'load':40s} {'peak dB':>8s} {'@kHz':>6s} {'Q~':>5s} {'20Hz-20k dB':>13s} {'@20k':>6s} {'min|Z| ohm':>10s}")
    for ck, cf in b["C"].items():
        zc = b["zob"][1][ "nominal" if ck == "nominal" else [k for k in b["zob"][1] if k != "nominal"][0]] if b["zob"] else None
        f0 = 1 / (2 * math.pi * math.sqrt(b["L"] * cf))
        for lk, ld in LOADS.items():
            H = [(f, *resp(b, cf, zc, ld, f)) for f in F if f <= 500e3]
            pk = max(H, key=lambda x: abs(x[1]))
            pkdb = 20 * math.log10(abs(pk[1]))
            band = [20 * math.log10(abs(h)) for f, h, z in H if 20 <= f <= 20e3]
            a20 = 20 * math.log10(abs(min(H, key=lambda x: abs(x[0] - 20e3))[1]))
            zmin = min(abs(z) for f, h, z in H if 20 <= f <= 20e3)
            q = 10 ** (pkdb / 20)
            print(f"{ck:24s} {lk:40s} {pkdb:+8.1f} {pk[0]/1e3:6.1f} {q:5.1f} {min(band):+6.2f}/{max(band):+5.2f} {a20:+6.2f} {zmin:10.2f}")
            if lk in ("4 ohm", "8 ohm"):
                if max(abs(min(band)), abs(max(band))) > 1.5: fails.append(f"{name} {ck} {lk}: band {min(band):+.2f}/{max(band):+.2f} dB")
                if zmin < 3.2: fails.append(f"{name} {ck} {lk}: min |Z| {zmin:.2f} ohm")
            else:
                if b["pk_lim"] is not None and pkdb > b["pk_lim"]:
                    fails.append(f"{name} {ck} {lk}: peak {pkdb:+.1f} dB > {b['pk_lim']}")
                if lk != "open":
                    zs = min(abs(ld(2 * math.pi * f)) for f in F if 20 <= f <= 20e3)
                    print(f"{'':24s}   speaker alone min |Z| {zs:.2f} ohm; filter takes it to {zmin:.2f} ({100*(zmin/zs-1):+.1f} %)")
                    if zmin < 0.95 * zs: fails.append(f"{name} {ck} {lk}: |Z| {zmin:.2f} vs speaker {zs:.2f}")
        print(f"{'':24s} (bare LC f0 {f0/1e3:.1f} kHz)")
    if b["zob"]:
        for ck, zc in b["zob"][1].items():
            f = 20e3; z = abs(b["zob"][0] + 1 / (1j * 2 * math.pi * f * zc)); p = (10.6 ** 2 / z ** 2) * b["zob"][0]
            print(f"  Zobel R (0603, 0.1 W) at a full-swing 10.6 Vrms/leg 20 kHz sine, C {ck}: {p:.2f} W (music: mW; firmware tone guard caps sines above 5 kHz at -16 dBFS)")
print()
print("FAIL: " + "; ".join(fails) if fails else "ALL PASS (lite open-load peak is the accepted no-Zobel risk, reported only)")
sys.exit(1 if fails else 0)
