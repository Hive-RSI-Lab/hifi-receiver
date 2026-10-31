"""R5-PA: Q1 (AO4485) through the PVDD soft start as built on main
(C4 100 nF G-D, C8 2.2 uF G-S, R4/R5 470k, bulk C5+C6+C311 = 1410 uF) and
with review-3 R3-POWER-2's hardware half (C4 200 nF, C8 4.4 uF), which the
amp ECO did not add. Testbench = docs/review-3/sims/power/pvdd/core_fw_on_28v.cir
(same models.inc), swept over Vth 1.7/1.9/2.5 V and VBUS 28/29.4 V. The FET
power waveform goes to hwde soa_check --csv at Ta 25/40/50/60 C.
Run with $HWDE_PY (needs HWDE_SKILL, HWDE_NGSPICE_DLL). Read-only.
Exit 1 if the as-built Tj passes 150 C at Ta 40 C (a warm re-plug)."""
import json, os, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.environ["HWDE_SKILL"], "scripts", "lib"))
from pathlib import Path
import simlib
simlib.setup_engine_env(Path(os.environ["HWDE_NGSPICE_DLL"]))
HERE = os.path.dirname(os.path.abspath(__file__))
MODELS = open(os.path.join(HERE, "models.inc")).read()

def tb(v, vth, c4, c8, pdstep=False):
    return f"""pvdd soft start
{MODELS.replace('.param vto_p=1.9', f'.param vto_p={vth}')}
Rcab vsrc vc 0.1
Lcab vc VBUS_IN 0.6u
R1 VBUS_IN VBUS 10m
C10 VBUS 0 3u
R17 VBUS VBUS_PD 10
C1 VBUS_PD 0 0.6u
Rbuck VBUS 0 150
M1 PVDDm PVDD_G VBUS AO4485
Vfetm PVDD PVDDm 0
R4 VBUS PVDD_G 470k
R5 PVDD_G PVDD_PD 470k
C4 PVDD_G PVDD {c4}
C8 VBUS PVDD_G {c8}
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
Ccer PVDD 0 5.3u
Ramp PVDD 0 14k
{SRC_STEP if pdstep else SRC_ON.format(v=v)}
.end
"""

SRC_ON = """Vsrc vsrc 0 {v}
Vpg vpg 0 3
Ven PVDD_EN 0 PWL(0 0 20m 0 20.01m 3.3)
.ic v(PVDD)=0 v(PVDD_G)={v} v(VBUS)={v} v(VBUS_IN)={v} v(VBUS_PD)={v}
.tran 100u 800m 0 100u uic"""
# review-3 core_pdstep_5to28_fwoff: 5 -> 28 V at 30 V/ms, PVDD_EN low, PG high
SRC_STEP = """Vsrc vsrc 0 PWL(0 5 10m 5 10.77m 28 300m 28)
Vpg vpg 0 0
Ven PVDD_EN 0 0
.ic v(PVDD)=0 v(PVDD_G)=5 v(VBUS)=5 v(VBUS_IN)=5 v(VBUS_PD)=5
.tran 5u 100m 0 5u uic"""

def soa(ts, ps, ta):
    with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as f:
        f.write("t,p\n")
        for t, p in zip(ts, ps):
            f.write(f"{t:.6g},{max(p, 0):.6g}\n")
        name = f.name
    out = subprocess.run([sys.executable, os.path.join(os.environ["HWDE_SKILL"], "scripts", "soa_check.py"),
                          "--fet", "AO4485", "--csv", name, "--ta", str(ta)], capture_output=True, text=True).stdout
    os.unlink(name)
    j = json.loads(out)
    return j["result"]["tj_peak_c"]

bad = 0
# effective values: C8 2.2u/50V 0805 ~1.8u (review-3); 2 x 100n (R3 fix); in-place swap
# C4 220n/50V 0603 X7R taken at 150n (DC bias at ~24 V), C8 4.7u/25V 0805 at 3.5u (~4 V)
VARIANTS = (("as built", "100n", "1.8u"), ("R3-POWER-2 fix", "200n", "3.6u"),
            ("swap 220n/4.7u", "150n", "3.5u"))
for label, c4, c8 in VARIANTS:
    r = simlib.run_vectors([simlib.prepare_circuit(tb(28, 1.7, c4, c8, pdstep=True))])
    pl = r["plots"][0]
    vsg = max(a - b for a, b in zip(pl["vbus"], pl["pvdd_g"]))
    print(f"{label:15s} PD step 5->28 V, PVDD_EN low, Vth 1.7: Vsg max {vsg:.2f} V, PVDD max {max(pl['pvdd']):.3f} V")
    if vsg > 1.5:
        bad = 1
for label, c4, c8 in VARIANTS:
    for v in (28.0, 29.4):
        for vth in (1.7, 1.9, 2.5):
            r = simlib.run_vectors([simlib.prepare_circuit(tb(v, vth, c4, c8))])
            if r["status"] != "ok":
                print("sim failed", r.get("error")); sys.exit(2)
            pl = r["plots"][0]
            t = list(pl["time"]); vb = list(pl["vbus"]); vp = list(pl["pvdd"]); i = list(pl["i(vfetm)"]) if "i(vfetm)" in pl else list(pl["vfetm#branch"])
            p = [(a - b) * (-c) for a, b, c in zip(vb, vp, i)]
            e = sum(0.5 * (p[k] + p[k + 1]) * (t[k + 1] - t[k]) for k in range(len(t) - 1))
            t90 = next((tt for tt, x in zip(t, vp) if x >= 0.9 * v), None)
            tjs = {ta: soa(t, p, ta) for ta in (25, 40, 50, 60)}
            print(f"{label:15s} VBUS {v:4.1f} Vth {vth}: P peak {max(p):5.1f} W, E {e:.3f} J, "
                  f"I peak {max(-x for x in i):.2f} A, t90 {1e3*(t90-0.02):.0f} ms after EN; Tj "
                  + ", ".join(f"{tj:.0f} C@{ta}" for ta, tj in tjs.items() if tj is not None))
            if label == "as built" and tjs[40] and tjs[40] > 150:
                bad = 1
sys.exit(bad)
