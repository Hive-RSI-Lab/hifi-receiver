"""Generate the review-3 PVDD-switch / PD-transition testbenches (current main values) with
bounds sidecars, for hwde sim_run.py:  $HWDE_PY $HWDE_SKILL/scripts/sim_run.py --dir <this dir>
Values from boards/*/kicad/*.net at ac484b1; MLCC values derated with hwde mlcc_dcbias (typ)."""
import json
from pathlib import Path
D = Path(__file__).parent

LITE = """* Lite PVDD switch as on main: Q1 AO4485, R5 100k G-S, R15 1k + C2 100n G-D (Miller), R6 220k,
* C19 4.7u/16V G-S (2.4u at its <=6.6 V bias), Q4 MMBT3904 E=PD_PG, R7 10k, R8 100k, R4 10k PG pull-up.
Rcab vsrc vc {rcab}
Lcab vc VBUS {lcab}
* VBUS ceramics: C1 1u/50V 0603 (0.47u at 21 V), C3 10u/50V 1206 (4.1u), C4 4.7u/50V 1206 (2.8u), C5 100n
Cvb VBUS 0 7.5u
Rbuck VBUS 0 {rbuck}
Rvs VBUS 0 110k
M1 PVDDm PSW_G VBUS AO4485
Vfetm PVDD PVDDm 0
R5 VBUS PSW_G 100k
R15 PSW_G PSW_MILLER 1k
C2 PSW_MILLER PVDD 100n
R6 PSW_G PSW_PULL 220k
C19 VBUS PSW_G 2.4u
Q4 PSW_PULL PSW_B PD_PG Q3904
R7 PVDD_EN PSW_B 10k
R8 PVDD_EN 0 100k
R4 PD_PG V3V3 10k
V3 V3V3 0 3.3
Spg PD_PG 0 vpg 0 SWPG
.model SWPG SW(Ron=30 Roff=1e9 Vt=1.5 Vh=0.1)
* PVDD bulk: C58 470u polymer 16 mR, C59 220u Al 0.3 R, 3x1u/50V 0805 (0.73u) + 2x100n + 2x1n, R16 22k, R17/R18 110k
C58 PVDD p58 470u
R58 p58 0 16m
C59 PVDD p59 220u
R59 p59 0 0.3
Ccer PVDD 0 2.4u
R16 PVDD 0 22k
Rps PVDD 0 110k
Ramp PVDD 0 {ramp}
"""

CORE = """* core PVDD switch as on main: Q1 AO4485, R4/R5 470k, C4 100n G-D, C8 2.2u/50V G-S, D4 15 V clamp,
* Q2 2N7002 (S = PD_PG), R6 1k, R7 100k, R404 100k + D5 BAS16 + Q3 unit 2 fast-off follower,
* D2 33 V + R8/R9 10k + Q3 unit 1 OV trip, R20 10k PG pull-up. Bulk C5, C6, C311 470u/50V PX (ESR ~0.2 R).
Rcab vsrc vc {rcab}
Lcab vc VBUS_IN {lcab}
R1 VBUS_IN VBUS 10m
* C10 10u/50V 1206 (2.6u at 28 V, 4.1u at 20 V) + C11 100n; R17 10R + C1 1u/50V 0805 (0.58u)
C10 VBUS 0 3u
R17 VBUS VBUS_PD 10
C1 VBUS_PD 0 0.6u
Rbuck VBUS 0 {rbuck}
M1 PVDDm PVDD_G VBUS AO4485
Vfetm PVDD PVDDm 0
R4 VBUS PVDD_G 470k
R5 PVDD_G PVDD_PD 470k
C4 PVDD_G PVDD 100n
C8 VBUS PVDD_G 1.8u
D4 PVDD_G VBUS DZ15
M2 PVDD_PD PVDD_ENG PD_PG N2N7002
R6 PVDD_EN PVDD_ENG 1k
R7 PVDD_ENG 0 100k
R404 VBUS PVDD_PD 100k
D5 PVDD_PD PVDD_OFFB DBAS16
Q3B VBUS PVDD_OFFB PVDD_G Q3904
D2 OV_Z VBUS DZ33
R8 OV_Z OV_B 10k
R9 OV_B 0 10k
Q3A PVDD_ENG OV_B 0 Q3904
R20 PD_PG V3V3 10k
V3 V3V3 0 3.3
Spg PD_PG 0 vpg 0 SWPG
.model SWPG SW(Ron=30 Roff=1e9 Vt=1.5 Vh=0.1)
C5 PVDD p5 470u
R5e p5 0 0.2
C6 PVDD p6 470u
R6e p6 0 0.2
C311 PVDD p311 470u
R311e p311 0 0.2
* C7 10u/50V 1206 (2.6u) + 4 x 1u/50V 0805 (0.58u) + 4 x 100n
Ccer PVDD 0 5.3u
Ramp PVDD 0 {ramp}
"""

def bench(name, title, inc, params, stim, tran, meas, bounds):
    txt = [title, ".include models.inc", inc.format(**params), stim, tran] + meas + [".end", ""]
    (D / f"{name}.cir").write_text("\n".join(txt))
    (D / f"{name}.bounds.json").write_text(json.dumps(bounds, indent=1) + "\n")

def b(m, lo=None, hi=None, msg="", sev="error"):
    d = {"measure": m, "severity": sev, "msg": msg}
    if lo is not None: d["min"] = lo
    if hi is not None: d["max"] = hi
    return d

FETM = [".meas tran ifet_pk max par('-i(Vfetm)') from={t0} to={t1}",
        ".meas tran pfet_pk max par('(v(VBUS)-v(PVDD))*(-i(Vfetm))') from={t0} to={t1}",
        ".meas tran efet integ par('(v(VBUS)-v(PVDD))*(-i(Vfetm))') from={t0} to={t1}",
        ".meas tran isrc_pk max i(Lcab) from={t0} to={t1}"]
def fm(t0, t1): return [m.format(t0=t0, t1=t1) for m in FETM]

# AO4485 SOA (AOS Rev1.1 fig 9, 25 C, single pulse): ~20 W for 100 ms, ~45 W for 10 ms.
# Energy gate: a 25 C start, Zth(10-100 ms) ~ 8-15 K/W (fig 11 x RthJA 75 K/W): E < 0.4 J keeps dTj well < 100 K.
lite_p = dict(rcab=0.1, lcab="0.6u", rbuck=240, ramp="1G")
for v in (20, 15):
    bench(f"lite_fw_on_{v}v", f"Lite: VBUS {v} V settled, PG good, firmware sets PVDD_EN at 20 ms: inrush/FET SOA",
          LITE, lite_p,
          f"Vsrc vsrc 0 {v}\nVpg vpg 0 3\nVen PVDD_EN 0 PWL(0 0 20m 0 20.01m 3.3)\n.ic v(PVDD)=0 v(PSW_G)={v} v(VBUS)={v}",
          ".tran 50u 1.2 0 50u uic",
          fm("20m", "1.2") + [f".meas tran t90 when v(PVDD)={0.9*v} rise=1",
                              ".meas tran vsg_end find par('v(VBUS)-v(PSW_G)') at=1.19"],
          [b("ifet_pk", hi=2.0, msg="inrush under 2 A (3 A contract)"),
           b("pfet_pk", hi=20, msg="FET peak power under the 100 ms SOA line (~20 W)"),
           b("efet", hi=0.4, msg="FET energy"),
           b("t90", hi=1.0, msg="PVDD at 90 % within the firmware's 1 s pvdd_timeout"),
           b("vsg_end", lo=3.5, msg="final gate drive (RDS(on) only specified >=4.5 V)", sev="warning")])
# PD step 5 -> 20 V at the PD max slew (30 mV/us), firmware off, PG high (no contract yet)
bench("lite_pdstep_5to20_fwoff", "Lite: 5->20 V PD step at 30 V/ms, PVDD_EN low: Q1 must stay off",
      LITE, lite_p,
      "Vsrc vsrc 0 PWL(0 5 10m 5 10.5m 20 300m 20)\nVpg vpg 0 0\nVen PVDD_EN 0 0\n.ic v(PVDD)=0 v(PSW_G)=5 v(VBUS)=5",
      ".tran 5u 300m 0 5u uic",
      fm("0", "300m") + [".meas tran vsg_max max par('v(VBUS)-v(PSW_G)') from=0 to=300m",
                         ".meas tran pvdd_max max v(PVDD) from=0 to=300m"],
      [b("vsg_max", hi=1.2, msg="Vsg under 1.7 V min Vth with margin"), b("pvdd_max", hi=1.0, msg="PVDD stays off")])
# hot plug at 5 V: charger already at 5 V, 1 m cable 1 uH, 0.05 R (low-R cable = worst ringing)
hp = dict(lite_p, rcab=0.05, lcab="1u")
bench("lite_hotplug_5v", "Lite: hot plug into a live 5 V source (1 uH, 50 mR cable), ceramic-only VBUS",
      LITE, hp,
      "Vsrc vsrc 0 PWL(0 0 10u 0 10.2u 5 50m 5)\nVpg vpg 0 0\nVen PVDD_EN 0 0",
      ".tran 0.1u 50m 0 0.1u",
      [".meas tran vbus_pk max v(VBUS) from=0 to=50m", ".meas tran vsg_max max par('v(VBUS)-v(PSW_G)') from=0 to=50m",
       ".meas tran pvdd_max max v(PVDD) from=0 to=50m"],
      [b("vbus_pk", hi=14, msg="overshoot well under every VBUS part's abs max (28-30 V)"),
       b("vsg_max", hi=1.2, msg="Q1 held off through the ring"), b("pvdd_max", hi=1.0, msg="PVDD stays off")])
# hot plug into a (non-compliant) live 20 V source: what the TVS and the parts see
hp20 = dict(hp)
bench("lite_hotplug_20v", "Lite: hot plug into a live 20 V source (non-compliant charger or adapter), 1 uH/50 mR",
      LITE + "D1 0 VBUS DTVS22\n", hp20,
      "Vsrc vsrc 0 PWL(0 0 10u 0 10.2u 20 50m 20)\nVpg vpg 0 0\nVen PVDD_EN 0 0",
      ".tran 0.1u 50m 0 0.1u",
      [".meas tran vbus_pk max v(VBUS) from=0 to=50m", ".meas tran vsg_max max par('v(VBUS)-v(PSW_G)') from=0 to=50m",
       ".meas tran pvdd_max max v(PVDD) from=0 to=50m"],
      [b("vbus_pk", hi=30, msg="TPS54302 VIN / TPA3118 abs max 30 V", sev="warning"),
       b("pvdd_max", hi=2.0, msg="PVDD stays off", sev="warning")])

core_p = dict(rcab=0.1, lcab="0.6u", rbuck=150, ramp="14k")
for v in (28, 20, 15):
    bench(f"core_fw_on_{v}v", f"core: VBUS {v} V settled, PG good, PVDD_EN at 20 ms: inrush into 1410 uF, FET SOA",
          CORE, core_p,
          f"Vsrc vsrc 0 {v}\nVpg vpg 0 3\nVen PVDD_EN 0 PWL(0 0 20m 0 20.01m 3.3)\n.ic v(PVDD)=0 v(PVDD_G)={v} v(VBUS)={v} v(VBUS_IN)={v} v(VBUS_PD)={v}",
          ".tran 50u 800m 0 50u uic",
          fm("20m", "800m") + [f".meas tran t90 when v(PVDD)={0.9*v} rise=1", f".meas tran t10 when v(PVDD)={0.1*v} rise=1",
                               ".meas tran vsg_end find par('v(VBUS)-v(PVDD_G)') at=790m"],
          [b("ifet_pk", hi=2.0, msg="inrush"), b("pfet_pk", hi=10, msg="FET peak <= 10 W: hwde soa_check Tj < 110 C at 50 C ambient (14.7 W -> 135 C)"),
           b("efet", hi=0.6, msg="FET energy"),
           b("t90", hi=0.12, msg="PVDD at 90 % (t from 0; PVDD_EN at 20 ms) before firmware soft_start_ms = 100 ms releases the amps"),
           b("vsg_end", lo=4.5, msg="gate drive")])
bench("core_pdstep_5to28_fwoff", "core: 5->28 V at 30 V/ms, PVDD_EN low, PG high: Q1 must stay off",
      CORE, core_p,
      "Vsrc vsrc 0 PWL(0 5 10m 5 10.77m 28 300m 28)\nVpg vpg 0 0\nVen PVDD_EN 0 0\n.ic v(PVDD)=0 v(PVDD_G)=5 v(VBUS)=5 v(VBUS_IN)=5 v(VBUS_PD)=5",
      ".tran 5u 300m 0 5u uic",
      fm("0", "300m") + [".meas tran vsg_max max par('v(VBUS)-v(PVDD_G)') from=0 to=300m",
                         ".meas tran pvdd_max max v(PVDD) from=0 to=300m"],
      [b("vsg_max", hi=1.5, msg="Vsg under 1.7 V min Vth (0.2 V guard)"), b("pvdd_max", hi=1.0, msg="PVDD stays off")])
# firmware enabled PVDD on the SPR 20 V contract, then the CH224Q enters EPR and VBUS steps to 28 V with Q1 on
bench("core_epr_step_20to28_on", "core: Q1 on at 20 V, then the EPR step to 28 V at 30 V/ms: source current into the bulk",
      CORE, core_p,
      "Vsrc vsrc 0 PWL(0 20 400m 20 400.27m 28 700m 28)\nVpg vpg 0 3\nVen PVDD_EN 0 PWL(0 0 20m 0 20.01m 3.3)\n.ic v(PVDD)=0 v(PVDD_G)=20 v(VBUS)=20 v(VBUS_IN)=20 v(VBUS_PD)=20",
      ".tran 20u 700m 0 20u uic",
      [".meas tran isrc_step max i(Lcab) from=399m to=700m", ".meas tran pfet_step max par('(v(VBUS)-v(PVDD))*(-i(Vfetm))') from=399m to=700m"],
      [b("isrc_step", hi=6.0, msg="the source must deliver this during its own slew; an ideal source here (a real one current-limits)", sev="warning"),
       b("pfet_step", hi=20, msg="FET fully on: no SOA stress")])
bench("core_hotplug_5v", "core: hot plug into a live 5 V source, 1 uH/50 mR cable, ceramic-only VBUS",
      CORE + "D1 0 VBUS_IN DTVS30\n", dict(core_p, rcab=0.05, lcab="1u"),
      "Vsrc vsrc 0 PWL(0 0 10u 0 10.2u 5 50m 5)\nVpg vpg 0 0\nVen PVDD_EN 0 0",
      ".tran 0.1u 50m 0 0.1u",
      [".meas tran vbus_pk max v(VBUS) from=0 to=50m", ".meas tran vpd_pk max v(VBUS_PD) from=0 to=50m",
       ".meas tran vsg_max max par('v(VBUS)-v(PVDD_G)') from=0 to=50m", ".meas tran pvdd_max max v(PVDD) from=0 to=50m"],
      [b("vbus_pk", hi=14, msg="overshoot"), b("vsg_max", hi=1.2, msg="Q1 held off"), b("pvdd_max", hi=1.0, msg="PVDD off")])
bench("core_hotplug_28v", "core: hot plug into a live 28 V source (non-compliant), 1 uH/50 mR: CH224Q VHV (32 V abs)",
      CORE + "D1 0 VBUS_IN DTVS30\n", dict(core_p, rcab=0.05, lcab="1u"),
      "Vsrc vsrc 0 PWL(0 0 10u 0 10.2u 28 50m 28)\nVpg vpg 0 0\nVen PVDD_EN 0 0",
      ".tran 0.1u 50m 0 0.1u",
      [".meas tran vbus_pk max v(VBUS) from=0 to=50m", ".meas tran vpd_pk max v(VBUS_PD) from=0 to=50m"],
      [b("vpd_pk", hi=32, msg="CH224Q VHV abs max 32 V", sev="warning")])
# PD hard reset with PVDD on: source drops VBUS (its own discharge ~ 200 R), PG releases; what VBUS does by tSafe0V
for brd, inc, p, gate in (("lite", LITE, dict(lite_p, ramp="1G"), "PSW_G"), ("core", CORE, dict(core_p, ramp="14k"), "PVDD_G")):
    v = 20 if brd == "lite" else 28
    ic = f".ic v(PVDD)={v} v(VBUS)={v}" + (f" v(PSW_G)={v*0.69:.2f}" if brd == "lite" else "") + ("" if brd == "lite" else f" v(VBUS_IN)={v} v(VBUS_PD)={v} v(PVDD_G)={v-14}")
    srcline = (f"Vsrc vsrco 0 PWL(0 {v} 10m {v} 10.1m 0 2 0)\nDsrc vsrco vsrc DIDEAL\nRdis vsrc 0 200\n")
    inc2 = inc.replace("Rcab vsrc vc", "Rcab vsrc vc")
    bench(f"{brd}_hardreset_pvdd_on", f"{brd}: PD hard reset with PVDD on (amp in shutdown): does VBUS reach vSafe0V (0.8 V) by tSafe0V (650 ms)?",
          inc2, p,
          srcline + "Vpg vpg 0 PWL(0 3 10m 3 10.01m 0)\nVen PVDD_EN 0 PWL(0 3.3 10m 3.3 10.01m 0)\n" + ic,
          ".tran 100u 1 0 100u uic",
          [".meas tran vbus_650 find v(VBUS) at=660m", ".meas tran pvdd_650 find v(PVDD) at=660m",
           ".meas tran ifet_rev max par('i(Vfetm)') from=10m to=1"],
          [b("vbus_650", hi=0.8, msg="USB PD tSafe0V/vSafe0V: VBUS back-fed from PVDD bulk via Q1 body diode", sev="warning")])
print("written")

# ---- proposed fix for R3-POWER core turn-on (C4 2 x 100n, C8 4.7u/50V): same benches
CORE_FIX = CORE.replace("C4 PVDD_G PVDD 100n", "C4 PVDD_G PVDD 200n").replace("C8 VBUS PVDD_G 1.8u", "C8 VBUS PVDD_G 3.8u")
bench("core_fw_on_28v_FIX", "core FIX (C4 200n, C8 4.7u): VBUS 28 V, PVDD_EN at 20 ms",
      CORE_FIX, core_p,
      "Vsrc vsrc 0 28\nVpg vpg 0 3\nVen PVDD_EN 0 PWL(0 0 20m 0 20.01m 3.3)\n.ic v(PVDD)=0 v(PVDD_G)=28 v(VBUS)=28 v(VBUS_IN)=28 v(VBUS_PD)=28",
      ".tran 50u 1.2 0 50u uic",
      fm("20m", "1.2") + [".meas tran t90 when v(PVDD)=25.2 rise=1", ".meas tran t10 when v(PVDD)=2.8 rise=1",
                          ".meas tran vsg_end find par('v(VBUS)-v(PVDD_G)') at=1.19"],
      [b("pfet_pk", hi=9, msg="FET peak power: soa_check Tj < 110 C at 50 C ambient"),
       b("t90", hi=0.5, msg="PVDD at 90 % inside a 500 ms firmware soft start"), b("vsg_end", lo=4.5, msg="gate drive")])
bench("core_pdstep_5to28_fwoff_FIX", "core FIX: 5->28 V at 30 V/ms, PVDD_EN low: Q1 stays off",
      CORE_FIX, core_p,
      "Vsrc vsrc 0 PWL(0 5 10m 5 10.77m 28 300m 28)\nVpg vpg 0 0\nVen PVDD_EN 0 0\n.ic v(PVDD)=0 v(PVDD_G)=5 v(VBUS)=5 v(VBUS_IN)=5 v(VBUS_PD)=5",
      ".tran 5u 300m 0 5u uic",
      fm("0", "300m") + [".meas tran vsg_max max par('v(VBUS)-v(PVDD_G)') from=0 to=300m",
                         ".meas tran pvdd_max max v(PVDD) from=0 to=300m"],
      [b("vsg_max", hi=1.5, msg="Vsg under 1.7 V min Vth"), b("pvdd_max", hi=1.0, msg="PVDD stays off")])
