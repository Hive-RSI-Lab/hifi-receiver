"""Power netlist rules for lite-main and core (review-3). Exit 1 on any fail. Read-only.
Checks the PD sink wiring against WCH's CH224 V2.0 fig 6.1.1, the PVDD switch's PG interlock,
buck set points against the datasheet equations, and EN handling."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from netlib import Net
fails = []
def chk(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond: fails.append(msg)
def rv(s):
    s = s.replace('R', '').strip(); m = {'k': 1e3, 'M': 1e6, 'm': 1e-3}
    return float(s[:-1]) * m[s[-1]] if s[-1] in m else float(s)

L = Net("/work/boards/lite-main/kicad/lite-main.net")
C = Net("/work/boards/core/kicad/core.net")
# CH224: VHV(1) and VBUS(8) on the same supply net, 1 uF on it, CFG1 resistor value, PG pulled up
for nm, n, vhv, rset, want in (("lite", L, "VBUS", "R1", 120e3), ("core", C, "VBUS_PD", "R2", 210e3)):
    chk(n.netof("U1", 1) == vhv and n.netof("U1", 8) == vhv, f"{nm} CH224 VHV and VBUS pins both on {vhv}")
    chk(n.netof("U1", 9) == "PD_CFG1" and abs(rv(n.value(rset)) - want) < 1, f"{nm} CFG1 Rset {n.value(rset)} (table 5-1)")
    chk("PD_PG" == n.netof("U1", 10), f"{nm} CH224 PG on PD_PG")
    chk(n.netof("U1", 11) == "GND", f"{nm} CH224 EP to GND")
# PG gates the PVDD switch driver in hardware
chk(L.netof("Q4", 2) == "PD_PG", "lite Q4 emitter on PD_PG (PVDD needs PG low)")
chk(set(C.pins_of("Q2").values()) >= {"PD_PG", "PVDD_PD", "PVDD_ENG"}, "core Q2 source on PD_PG")
# buck set points
vl = 0.596 * (1 + rv(L.value("R9")) / rv(L.value("R10")))
chk(4.9 < vl < 5.3, f"lite TPS54302 +5V = {vl:.2f} V (VFB 0.596)")
vc = 0.6 * (1 + rv(C.value("R13")) / rv(C.value("R14")))
chk(4.9 < vc < 5.3, f"core SY8303 +5V = {vc:.2f} V")
fs = 105 / (rv(C.value("R12")) / 1e3)
chk(0.5 <= fs <= 2.5, f"core SY8303 fsw = {fs:.2f} MHz; min on-time at 29.4 V = {5.1/29.4/fs*1e3:.0f} ns vs 80 ns")
for nm, n, r1, r2 in (("lite", L, "R11", "R12"), ("core", C, "R15", "R16")):
    v3 = 0.6 * (1 + rv(n.value(r1)) / rv(n.value(r2)))
    chk(3.2 < v3 < 3.45, f"{nm} SY8089 +3V3 = {v3:.2f} V")
# EN handling: TPS54302 floats (allowed), SY8303 must not float
chk(L.netof("U2", 5).startswith("unconnected"), "lite TPS54302 EN floats (datasheet: float = enabled; abs max 7 V so never to VBUS)")
en = [p for p, v in C.pins_of("U4").items() if v == "BUCK5_EN"]
chk(bool(en) and len(C.refs_on("BUCK5_EN")) == 3, "core SY8303 EN on a VBUS divider (R10/R11), not floating")
# TVS on the connector side
chk("D1" in L.refs_on("VBUS"), "lite D1 TVS on VBUS"); chk("D1" in C.refs_on("VBUS_IN"), "core D1 TVS on VBUS_IN")
# PVDD bulk count
chk(len([r for r in C.refs_on("PVDD") if "470" in C.value(r)]) == 3, "core PVDD bulk = 3 x 470 uF (1410 uF; sims assume this)")
print("FAIL" if fails else "PASS"); sys.exit(1 if fails else 0)
