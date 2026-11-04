"""Lite-main thermal pre-order check (analytic, ESTIMATES throughout; nothing measured or run on hardware).
Inputs: netlist refs (L1-L6, U8, Q1) via LCSC records; theta_JA and amp loss from docs/review-2/thermal.md (TI SLOS708G fig 14/16, SLVSDG6C, Silergy, AOS)
with 1/8-power loads from the amp-loss table there.  Ambient: box air 45 C (reviewer default), 52 C at 1/8 P in a 30 C room, 62 C governed max (review-2 thermal.md s6).
Run: eval "$(~/modules/bin/hwde env)"; heavy --quick $HWDE_PY docs/sims/lite/thermal.py > docs/sims/lite/out_thermal.txt"""
import sys, math
FAILS = []; KNOWN = []; THIN = []
def judge(name, val, lim, unit="K", known=None, note=""):
    ok = val <= lim; m = (lim - val) / abs(lim) * 100
    tag = "PASS" if ok else ("FAIL(known)" if known else "FAIL")
    if not ok: (KNOWN if known else FAILS).append((name, val, lim, known))
    elif m < 15: THIN.append((name, val, lim, m))
    print(f"  {name:<58s} {val:7.1f} {unit:<2s} limit <= {lim:<6g} margin {m:6.1f} %  {tag}  {note}")

# --- U8 TPA3118D2 (4 ohm, PVDD 20 V; 1/8 of P1% = 38 W/ch -> 9.5 W both ch per review-2 table) ---
TRIP = 150.0; MARG = 135.0           # trip 150 C+ (SLOS708G 6.6); require 15 K under it
LOSS = {  # case: (system loss W, inductor DCR W [L1-4], chip W) from review-2 s2.2 (fig 16 efficiency, estimate)
    "idle": (0.6, 0.0, 0.6),
    "1/8 P 4 ohm": (2.1, 0.14, 1.75),
    "1/8 P 8 ohm": (1.0, 0.05, 0.85),
    "full 4 ohm 1% (rated)": (10.4, 1.14, 9.0),
    "full 8 ohm 1% (rated)": (4.5, 0.3, 4.2),
}
THETA = {"28 C/W own estimate": 28.0, "35 C/W upper": 35.0, "47.5 C/W hwde check_thermal (pessimistic)": 47.5}
print("== 1. U8 TPA3118D2 Tj = Tair + P_chip x theta_JA   (limit: 135 C = trip 150 C minus 15 K)")
for case, amb in (("idle", 45), ("1/8 P 4 ohm", 45), ("1/8 P 4 ohm", 52), ("1/8 P 8 ohm", 52), ("1/8 P 4 ohm", 62),
                  ("full 4 ohm 1% (rated)", 45), ("full 8 ohm 1% (rated)", 45)):
    chip = LOSS[case][2]
    for tn, th in THETA.items():
        tj = amb + chip * th
        full = case.startswith("full")
        # full-power continuous is known to trip (THERMAL-3, governor); the 47.5 pessimistic column on 1/8 P is a flagged row too
        judge(f"U8 {case} Ta {amb} C, P {chip:.2f} W, {tn}", tj, MARG, "C",
              known="continuous rated power trips the amp; firmware governor limits (review-2 THERMAL-3)" if full else None)
print("  max sustainable chip loss at 52 C, 28 C/W, Tj 135 C: %.2f W ; at 47.5 C/W: %.2f W" % ((135-52)/28, (135-52)/47.5))

# --- regulators (loss from rails.py budget, estimated efficiencies) ---
print("\n== 2. Regulators (loss from efficiency estimate; theta_JA from datasheet notes via review-2; limit 125 C operating)")
V5, V3 = 5.181, 3.327
for tag, io3, io5, i4 in (("typ", 0.142, 0.127, 0.025), ("max (all peaks coincident)", 0.543, 0.428, 0.040)):
    p3 = V3 * io3 * (1 / 0.90 - 1)              # SY8089 eta 0.90
    p5 = V5 * io5 * (1 / 0.85 - 1)              # TPS54302 eta 0.85
    p4 = (V5 * 1.03 - 3.3) * i4
    for amb in (52, 62):
        judge(f"U2 TPS54302 {tag}: {p5:.2f} W x 118.9 C/W Ta {amb}", amb + p5 * 118.9, 125, "C")
        judge(f"U3 SY8089 {tag}: {p3:.2f} W x 120 C/W Ta {amb}", amb + p3 * 120, 125, "C")
        judge(f"U4 LP5907 {tag}: {p4:.3f} W x 193.4 C/W Ta {amb}", amb + p4 * 193.4, 125, "C")

# --- Q1 ---
print("\n== 3. Q1 AO4485 I2R (RDS(on) 12.5 mohm max at 10 V/25 C from LCSC C5125949, x1.3 hot; Vgs -13.6 V; ~70 C/W drain copper, estimate)")
rds = 0.0125 * 1.3
for case, i in (("1/8 P 4 ohm", 0.6), ("full 4 ohm 1%", 4.4)):
    p = i * i * rds
    for amb in (52, 62):
        judge(f"Q1 {case}: {i} A, {p*1e3:.0f} mW, Ta {amb}", amb + p * 70, 150, "C")

# --- inductors ---
print("\n== 4. Inductors: DCR x Irms^2 plus core-loss ESTIMATE (no vendor core-loss data); rise from rated-dT (40 K at Irms rating) thermal resistance; limit 125 C")
L = {  # ref group: (DCR ohm, Irms rating A at dT 40 K, Isat A, Irms case A, core loss W each estimate)
    "L1-L4 XR0850 10 uH (BD mode, 4 ohm)": (0.030, 6.5, 8.0, {"1/8 P": (1.09, 0.05), "full": (3.08, 0.10)}),
    "L5 SWPA6045S 10 uH (U2, 20 V)": (0.048, 2.45, 3.2, {"max load": (0.62, 0.02)}),
    "L6 VLCF4020 2.2 uH (U3)": (0.040, 1.85, 3.4, {"max load": (0.57, 0.02)}),
}
for name, (dcr, ir, isat, cases) in L.items():
    rth = 40.0 / (ir ** 2 * dcr)                # K/W implied by the rating
    for case, (irms, core) in cases.items():
        pd = irms ** 2 * dcr; pt = pd + core
        for amb in (52, 62):
            judge(f"{name} {case}: DCR {pd*1e3:.0f} mW + core {core*1e3:.0f} mW (est), Ta {amb}", amb + pt * rth, 125, "C",
                  known="continuous full power not reachable, amp trips first" if case == "full" else None)
print("  Core loss figures are estimates (ferrite 8x8 at ~400 kHz BD-mode ripple); mark these rows as estimates.")

print("\n== SUMMARY")
print("FAILS:", [f"{n}: {v:.4g} vs {l:g}" for n, v, l, k in FAILS] or "none")
print("KNOWN-LIMIT (expected, governed):", [f"{n}: {v:.4g} vs {l:g}" for n, v, l, k in KNOWN] or "none")
print("THIN (<15 %):", [f"{n}: {v:.4g} vs {l:g} ({m:.1f} %)" for n, v, l, m in THIN] or "none")
sys.exit(1 if FAILS else 0)
