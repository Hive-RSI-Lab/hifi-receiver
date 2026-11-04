"""core PVDD switch (Q1 AO4485), current copper (vals.py asserts every value below
against core.net). Re-runs review-5 pvdd_softstart.py / review-3 gen.py benches with
C4 220 nF and C8 4.7 uF at their DC-bias-derated values (hwde mlcc_dcbias):
  C4 220n 0603 X7R 50 V at 28 V: typ 152 nF, low 125 nF (also run at nominal 220 nF)
  C8 4.7u 0805 X5R 25 V at ~2 V (Vsg in hold-off): 4.6 uF; at 11 V (Vsg on): 2.8 uF
  PVDD ceramics 4x1u + 4x100n + C7 10u 1206 at 28 V: 5.3 uF; VBUS C10+C11: 3 uF; C1 0.6 uF
Checks:
  A soft start, PVDD_EN at 20 ms: t90 inside the 500 ms firmware soft start
    (firmware/components/hifi/core/hifi_power.c, soft_start_ms 500); Q1 Tj (hwde soa_check)
    under 150 C at Ta 25..60 C; sweep Vth 1.7/1.9/2.5 V and VBUS 28/29.4 V.
  B PD step 5->28 V at 30 V/ms with PVDD_EN low: Vsg max < 1.5 V (Q1 stays off).
  C PD hard reset with PVDD on (amp in shutdown): VBUS < 0.8 V (vSafe0V) at 650 ms.
  D hot plug into a live 28 V source (1 uH/50 mR cable): VBUS_PD (CH224Q VHV) < 32 V.
Run: $HWDE_PY docs/sims/core/pvdd_switch.py  (needs HWDE_SKILL, HWDE_NGSPICE_DLL).
Exit 1 on any fail. Simulation only; nothing here has been measured on hardware."""
import json, os, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, os.path.join(os.environ["HWDE_SKILL"], "scripts", "lib"))
import simlib
simlib.setup_engine_env(Path(os.environ["HWDE_NGSPICE_DLL"]))
HERE = Path(__file__).resolve().parent
MODELS = (HERE / "models.inc").read_text()

def tb(src, vth=1.9, c4="152n", c8="4.6u", cab=("0.1", "0.6u"), tvs=False):
    return f"""core pvdd switch
{MODELS.replace('.param vto_p=1.9', f'.param vto_p={vth}')}
Rcab vsrc vc {cab[0]}
Lcab vc VBUS_IN {cab[1]}
{"D1 0 VBUS_IN DTVS30" if tvs else ""}
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
R43 PVDD_EN 0 100k
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
{src}
.end
"""

def run(txt):
    r = simlib.run_vectors([simlib.prepare_circuit(txt)])
    if r["status"] != "ok":
        print("sim failed", r.get("error")); sys.exit(2)
    pl = r["plots"][0]
    return {k: list(v) for k, v in pl.items()}

def soa(ts, ps, ta):
    with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, dir=os.environ.get("TMPDIR")) as f:
        f.write("t,p\n")
        for t, p in zip(ts, ps): f.write(f"{t:.6g},{max(p, 0):.6g}\n")
        name = f.name
    out = subprocess.run([sys.executable, os.path.join(os.environ["HWDE_SKILL"], "scripts", "soa_check.py"),
                          "--fet", "AO4485", "--csv", name, "--ta", str(ta)], capture_output=True, text=True).stdout
    os.unlink(name)
    return json.loads(out)["result"]["tj_peak_c"]

bad = []
print("A  soft start (PVDD_EN at 20 ms), limit t90 < 500 ms after EN, Tj < 150 C")
worst = None
for c4 in ("220n", "152n", "125n"):
    for v in (28.0, 29.4):
        for vth in (1.7, 1.9, 2.5):
            src = (f"Vsrc vsrc 0 {v}\nVpg vpg 0 3\nVen PVDD_EN 0 PWL(0 0 20m 0 20.01m 3.3)\n"
                   f".ic v(PVDD)=0 v(PVDD_G)={v} v(VBUS)={v} v(VBUS_IN)={v} v(VBUS_PD)={v}\n"
                   ".tran 100u 900m 0 100u uic")
            pl = run(tb(src, vth, c4, "2.8u"))
            t, vb, vp, vg = pl["time"], pl["vbus"], pl["pvdd"], pl["pvdd_g"]
            i = pl.get("i(vfetm)") or pl["vfetm#branch"]
            p = [(a - b) * (-c) for a, b, c in zip(vb, vp, i)]
            e = sum(0.5 * (p[k] + p[k + 1]) * (t[k + 1] - t[k]) for k in range(len(t) - 1))
            t90 = next((tt for tt, x in zip(t, vp) if x >= 0.9 * v), None)
            tjs = {ta: soa(t, p, ta) for ta in (25, 40, 50, 60)}
            vsg_end = vb[-1] - vg[-1]
            t90ms = 1e3 * (t90 - 0.02) if t90 else float("inf")
            print(f"   C4 {c4:4s} VBUS {v:4.1f} Vth {vth}: P pk {max(p):5.1f} W  E {e:.3f} J  I pk {max(-x for x in i):.2f} A  "
                  f"t90 {t90ms:4.0f} ms  Vsg end {vsg_end:5.2f} V  Tj " + " ".join(f"{tjs[ta]:.0f}@{ta}" for ta in tjs))
            if t90ms > 500: bad.append(f"t90 {t90ms:.0f} ms C4 {c4} {v} V Vth {vth}")
            if tjs[60] > 150: bad.append(f"Tj {tjs[60]:.0f} C @60 C C4 {c4} {v} V Vth {vth}")
            if vsg_end < 4.5: bad.append(f"Vsg end {vsg_end:.2f} V")
            if worst is None or tjs[60] > worst[0]: worst = (tjs[60], c4, v, vth, t90ms, max(p), e)
print(f"   worst Tj@60 C: {worst[0]:.0f} C (C4 {worst[1]} VBUS {worst[2]} Vth {worst[3]}, P pk {worst[5]:.1f} W, E {worst[6]:.2f} J); "
      f"margin {150 - worst[0]:.0f} K")

print("B  PD step 5->28 V at 30 V/ms, PVDD_EN low, PG low; limit Vsg < 1.5 V (Vth min 1.7)")
for c4, c8 in (("220n", "4.6u"), ("125n", "4.6u"), ("125n", "2.8u")):
    src = ("Vsrc vsrc 0 PWL(0 5 10m 5 10.77m 28 300m 28)\nVpg vpg 0 0\nVen PVDD_EN 0 0\n"
           ".ic v(PVDD)=0 v(PVDD_G)=5 v(VBUS)=5 v(VBUS_IN)=5 v(VBUS_PD)=5\n.tran 5u 100m 0 5u uic")
    pl = run(tb(src, 1.7, c4, c8))
    vsg = max(a - b for a, b in zip(pl["vbus"], pl["pvdd_g"]))
    print(f"   C4 {c4} C8 {c8}: Vsg max {vsg:.2f} V  PVDD max {max(pl['pvdd']):.3f} V")
    if vsg > 1.5: bad.append(f"PD step Vsg {vsg:.2f} V")

print("C  PD hard reset with PVDD on, amp shut down (Ramp 14k), source discharge 200 R; limit VBUS < 0.8 V at 650 ms")
for c4, c8 in (("152n", "2.8u"), ("220n", "4.6u")):
    v = 28
    src = (f"Vsrc vsrco 0 PWL(0 {v} 10m {v} 10.1m 0 2 0)\nDsrc vsrco vsrc DIDEAL\nRdis vsrc 0 200\n"
           "Vpg vpg 0 PWL(0 3 10m 3 10.01m 0)\nVen PVDD_EN 0 PWL(0 3.3 10m 3.3 10.01m 0)\n"
           f".ic v(PVDD)={v} v(VBUS)={v} v(VBUS_IN)={v} v(VBUS_PD)={v} v(PVDD_G)={v-14}\n.tran 100u 1 0 100u uic")
    pl = run(tb(src, 1.9, c4, c8))
    t = pl["time"]
    k = min(range(len(t)), key=lambda j: abs(t[j] - 0.66))
    print(f"   C4 {c4} C8 {c8}: VBUS at 650 ms after reset {pl['vbus'][k]:.3f} V, PVDD {pl['pvdd'][k]:.2f} V")
    if pl["vbus"][k] > 0.8: bad.append(f"hard reset VBUS {pl['vbus'][k]:.2f} V")

print("D  hot plug into live 28 V, 1 uH / 50 mR cable, SMBJ30A on VBUS_IN; limit VBUS_PD < 32 V (CH224Q VHV abs max)")
src = "Vsrc vsrc 0 PWL(0 0 10u 0 10.2u 28 50m 28)\nVpg vpg 0 0\nVen PVDD_EN 0 0\n.tran 0.1u 20m 0 0.1u"
pl = run(tb(src, 1.7, "125n", "4.6u", cab=("0.05", "1u"), tvs=True))
vpd = max(pl["vbus_pd"]); vbk = max(pl["vbus"]); vsg = max(a - b for a, b in zip(pl["vbus"], pl["pvdd_g"]))
print(f"   VBUS pk {vbk:.1f} V  VBUS_PD pk {vpd:.2f} V  Vsg max {vsg:.2f} V  PVDD max {max(pl['pvdd']):.2f} V")
if vpd > 32: bad.append(f"hotplug VBUS_PD {vpd:.1f} V")

print("FAIL: " + "; ".join(bad) if bad else "ALL PASS")
sys.exit(1 if bad else 0)
