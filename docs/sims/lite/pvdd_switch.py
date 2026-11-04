"""Lite-main PVDD switch (Q1 AO4485) pre-order bench: turn-on, PD step hold-off, hot plug, PD hard reset.
Component values: those asserted by vals.py against boards/lite-main/kicad/lite-main.net.
Run:  eval "$(~/modules/bin/hwde env)"; heavy --quick $HWDE_PY docs/sims/lite/pvdd_switch.py > docs/sims/lite/out_pvdd_switch.txt
Corners: AO4485 Vth 1.7/1.9/2.5 V (models.inc .param vto_p), MLCC DC-bias typ/low (hwde mlcc_dcbias),
bulk caps nominal.  Q1 Tj from hwde soa_check (AO4485 SOA/Zth table, single pulse, Ta 25/40/50/60).
Nothing here has run on hardware; SPICE VDMOS model and the generic MLCC curve are estimates."""
import os, sys, json, subprocess, tempfile, re
from pathlib import Path
HERE = Path(__file__).resolve().parent
SK = Path(os.environ["HWDE_SKILL"]); PY = os.environ.get("HWDE_PY", sys.executable)
sys.path.insert(0, str(SK / "scripts/lib")); sys.path.insert(0, str(SK / "scripts"))
import simlib
simlib.setup_engine_env(Path(os.environ["HWDE_NGSPICE_DLL"]))
TMP = Path(os.environ.get("TMPDIR", tempfile.gettempdir())) / "lite_pvdd"; TMP.mkdir(parents=True, exist_ok=True)
MODELS = (HERE / "models.inc").read_text()

# ---------- MLCC derating (hwde mlcc_dcbias) ----------
_cache = {}
def ceff(c, pkg, vdc, rated, diel="X5R"):
    k = (c, pkg, round(vdc, 2), rated, diel)
    if k not in _cache:
        r = subprocess.run([PY, str(SK / "scripts/mlcc_dcbias.py"), "--c", c, "--package", pkg, "--rated", str(rated),
                            "--vdc", str(vdc), "--dielectric", diel], capture_output=True, text=True)
        p = json.loads(r.stdout)["part"]; _cache[k] = (p["c_eff"], p["c_eff_low"])
    return _cache[k]

def caps(corner, vbus):
    i = 0 if corner == "typ" else 1
    cvb = (ceff("1u", "0603", vbus, 50)[i] + ceff("10u", "1206", vbus, 50)[i]
           + ceff("4.7u", "1206", vbus, 50, "X7R")[i] + 0.1e-6)            # C1 C3 C4 C5
    ccer = 3 * ceff("1u", "0805", vbus, 50, "X7R")[i] + 0.2e-6 + 2e-9     # C51 C52 C55, C53 C56, C54 C57
    c19 = ceff("4.7u", "0603", 6.6, 16)[i]                                 # gate cap at its max Vsg bias (conservative)
    return cvb, ccer, c19

NET = """Rcab vsrc vc {rcab}
Lcab vc VBUS {lcab}
Cvb VBUS 0 {cvb}
Rbuck VBUS 0 240
Rvs VBUS 0 110k
M1 PVDDm PSW_G VBUS AO4485
Vfetm PVDD PVDDm 0
R5 VBUS PSW_G 100k
R15 PSW_G PSW_MILLER 1k
C2 PSW_MILLER PVDD 100n
R6 PSW_G PSW_PULL 220k
C19 VBUS PSW_G {c19}
Q4 PSW_PULL PSW_B PD_PG Q3904
R7 PVDD_EN PSW_B 10k
R8 PVDD_EN 0 100k
R4 PD_PG V3V3 10k
V3 V3V3 0 3.3
Spg PD_PG 0 vpg 0 SWPG
.model SWPG SW(Ron=30 Roff=1e9 Vt=1.5 Vh=0.1)
C58 PVDD p58 470u
R58 p58 0 16m
C59 PVDD p59 220u
R59 p59 0 0.3
Ccer PVDD 0 {ccer}
R16 PVDD 0 22k
Rps PVDD 0 110k
"""

def sim(title, stim, tran, corner, vcap, vto=1.9, rcab=0.1, lcab="0.6u", tvs=False):
    cvb, ccer, c19 = caps(corner, vcap)
    models = MODELS.replace(".param vto_p=1.9", f".param vto_p={vto}")
    assert f"vto_p={vto}" in models
    net = NET.format(rcab=rcab, lcab=lcab, cvb=cvb, c19=c19, ccer=ccer) + ("D1 0 VBUS DTVS22\n" if tvs else "")
    txt = "\n".join([title, models, net, stim, tran, ".end"])
    r = simlib.run_vectors([simlib.prepare_circuit(txt)])
    if r["status"] != "ok": raise SystemExit(f"sim failed: {r.get('error')}")
    import numpy as np
    return {k: np.array(v) for k, v in r["plots"][0].items()}

import numpy as np
def soa(t, p, ta, tag):
    f = TMP / f"{tag}.csv"
    with open(f, "w") as fh:
        fh.write("t,p\n")
        for a, b in zip(t, p): fh.write(f"{a:.6g},{max(b,0):.6g}\n")
    r = subprocess.run([PY, str(SK / "scripts/soa_check.py"), "--fet", "AO4485", "--csv", str(f), "--ta", str(ta)],
                       capture_output=True, text=True)
    try: j = json.loads(r.stdout)
    except Exception: raise SystemExit(f"soa_check failed: {r.stdout[-400:]} {r.stderr[-400:]}")
    return j

FAILS = []; THIN = []
def judge(name, val, lim, kind, unit="", sev="error"):
    """kind 'max': val<=lim passes; 'min': val>=lim passes.  Margin in % of limit."""
    ok = val <= lim if kind == "max" else val >= lim
    m = (lim - val) / abs(lim) * 100 if kind == "max" else (val - lim) / abs(lim) * 100
    tag = "PASS" if ok else ("FAIL" if sev == "error" else "WARN")
    if not ok: FAILS.append((name, val, lim, sev))
    elif m < 15: THIN.append((name, val, lim, m))
    print(f"  {name:<34s} {val:10.4g} {unit:<3s} limit {'<=' if kind=='max' else '>='} {lim:<8g} margin {m:7.1f} %  {tag}")

# ---------- 1. turn-on ----------
print("== 1. Turn-on, VBUS settled, PG good, PVDD_EN at 20 ms (Rcab 0.1, Lcab 0.6 uH, buck 240 R)")
print("   limits: I<=2 A (3 A contract), P<=20 W (AO4485 100 ms SOA @25C), E<=0.4 J, t90<=1 s (fw pvdd_timeout), Vsg_end>=3.5 V (warn), Tj<=150 C (soa_check)")
rows = []
for v in (20, 15):
    for corner in ("typ", "low"):
        for vto in (1.7, 1.9, 2.5):
            w = sim(f"lite turn-on {v}V", f"Vsrc vsrc 0 {v}\nVpg vpg 0 3\nVen PVDD_EN 0 PWL(0 0 20m 0 20.01m 3.3)\n.ic v(PVDD)=0 v(PSW_G)={v} v(VBUS)={v}",
                    ".tran 50u 1.2 0 50u uic", corner, v, vto=vto)
            t = w["time"]; i = -w["vfetm#branch"]; vds = w["vbus"] - w["pvdd"]; p = vds * i
            sel = t >= 20e-3
            ts, ps, is_, vs = t[sel], p[sel], i[sel], vds[sel]
            e = float(np.trapezoid(ps, ts)) if hasattr(np, "trapezoid") else float(np.trapz(ps, ts))
            idx = np.where(w["pvdd"] >= 0.9 * v)[0]
            t90 = float(t[idx[0]]) if len(idx) else float("inf")
            vsg_end = float((w["vbus"] - w["psw_g"])[t <= 1.19][-1])
            # soa window: from 20 ms until power < 1 mW after the peak
            k = int(np.argmax(ps)); tail = np.where(ps[k:] < 1e-3)[0]; end = k + (tail[0] if len(tail) else len(ps) - k - 1)
            tw, pw = ts[:end + 1] - ts[0], ps[:end + 1]
            tj = {}
            for ta in (25, 40, 50, 60):
                j = soa(tw, pw, ta, f"on{v}_{corner}_{vto}_{ta}")
                res = j.get("result", {})
                tj[ta] = res.get("tj_c", res.get("Tj", res.get("tj_peak_c")))
            rows.append((v, corner, vto, float(is_.max()), float(ps.max()), e, t90, vsg_end, tj, j))
            print(f"-- {v} V  MLCC {corner:<3s}  Vth {vto}")
            judge("I peak", float(is_.max()), 2.0, "max", "A")
            judge("P peak", float(ps.max()), 20.0, "max", "W")
            judge("energy", e, 0.4, "max", "J")
            judge("t90", t90, 1.0, "max", "s")
            judge("Vsg end", vsg_end, 3.5, "min", "V", sev="warn")
            print("  Tj by Ta (soa_check):", {k2: (round(x, 1) if isinstance(x, (int, float)) else x) for k2, x in tj.items()})
            for ta, x in tj.items():
                if isinstance(x, (int, float)): judge(f"Tj @Ta {ta}", x, 150.0, "max", "C")
print("  soa_check sample result keys:", list(rows[0][9].get("result", {}).keys()), "verdict:", rows[0][9].get("status"))

# ---------- 2. PD 5 -> 20 V step, firmware off ----------
print("\n== 2. PD 5->20 V step at 30 V/ms, PVDD_EN low, PG released (no contract): Q1 must stay off")
for corner in ("typ", "low"):
    for vto in (1.7, 1.9, 2.5):
        w = sim("pdstep", "Vsrc vsrc 0 PWL(0 5 10m 5 10.5m 20 300m 20)\nVpg vpg 0 0\nVen PVDD_EN 0 0\n.ic v(PVDD)=0 v(PSW_G)=5 v(VBUS)=5",
                ".tran 5u 300m 0 5u uic", corner, 20, vto=vto)
        vsg = float((w["vbus"] - w["psw_g"]).max()); pv = float(w["pvdd"].max())
        print(f"-- MLCC {corner}, Vth {vto}: Vsg max {vsg:.3f} V, PVDD max {pv*1e3:.2f} mV")
        judge("Vsg max vs Vth min 1.7 V", vsg, 1.7, "max", "V")
        judge("PVDD stays off (<1 V)", pv, 1.0, "max", "V")

# ---------- 3. hot plug ----------
print("\n== 3. Hot plug into a live source (Rcab 0.05, Lcab 1 uH; also 2 uH), ceramics only on VBUS, Q1 off")
for lc in ("1u", "2u"):
    for corner in ("typ", "low"):
        w = sim("hp5", "Vsrc vsrc 0 PWL(0 0 10u 0 10.2u 5 50m 5)\nVpg vpg 0 0\nVen PVDD_EN 0 0",
                ".tran 0.1u 50m 0 0.1u", corner, 5, rcab=0.05, lcab=lc)
        print(f"-- 5 V, Lcab {lc}, MLCC {corner}")
        judge("VBUS peak", float(w["vbus"].max()), 14.0, "max", "V")
        judge("Vsg max", float((w["vbus"] - w["psw_g"]).max()), 1.2, "max", "V")
        judge("PVDD max", float(w["pvdd"].max()), 1.0, "max", "V")
        w = sim("hp20", "Vsrc vsrc 0 PWL(0 0 10u 0 10.2u 20 50m 20)\nVpg vpg 0 0\nVen PVDD_EN 0 0",
                ".tran 0.1u 50m 0 0.1u", corner, 20, rcab=0.05, lcab=lc, tvs=True)
        print(f"-- 20 V, Lcab {lc}, MLCC {corner}, D1 SMBJ22A (DTVS22 model)")
        judge("VBUS peak vs 30 V abs max", float(w["vbus"].max()), 30.0, "max", "V", sev="warn")
        judge("Vsg max", float((w["vbus"] - w["psw_g"]).max()), 1.7, "max", "V")
        judge("PVDD max", float(w["pvdd"].max()), 2.0, "max", "V")
    w = sim("hp20n", "Vsrc vsrc 0 PWL(0 0 10u 0 10.2u 20 50m 20)\nVpg vpg 0 0\nVen PVDD_EN 0 0",
            ".tran 0.1u 50m 0 0.1u", "typ", 20, rcab=0.05, lcab=lc, tvs=False)
    print(f"-- 20 V, Lcab {lc}, MLCC typ, WITHOUT D1 (reference, shows what the TVS saves)")
    print(f"  VBUS peak {w['vbus'].max():.1f} V (info)")

# ---------- 4. PD hard reset ----------
print("\n== 4. PD hard reset with PVDD on: source drops VBUS at 10 ms (own discharge 200 R), PG/EN fall; vSafe0V 0.8 V by 650 ms")
for v in (20, 15):
    for corner in ("typ", "low"):
        w = sim("hr", f"Vsrc vsrco 0 PWL(0 {v} 10m {v} 10.1m 0 2 0)\nDsrc vsrco vsrc DIDEAL\nRdis vsrc 0 200\n"
                "Vpg vpg 0 PWL(0 3 10m 3 10.01m 0)\nVen PVDD_EN 0 PWL(0 3.3 10m 3.3 10.01m 0)\n"
                f".ic v(PVDD)={v} v(VBUS)={v} v(PSW_G)={v*0.69:.2f}", ".tran 100u 1 0 100u uic", corner, v)
        t = w["time"]; vb = float(np.interp(0.65, t, w["vbus"])); vp = float(np.interp(0.65, t, w["pvdd"]))
        print(f"-- {v} V, MLCC {corner}: PVDD at 650 ms {vp:.2f} V")
        judge("VBUS at 650 ms", vb, 0.8, "max", "V")

print("\n== SUMMARY")
print("FAILS:", [f"{n}: {v:.4g} vs {l:g} ({s})" for n, v, l, s in FAILS] or "none")
print("THIN (<15 %):", [f"{n}: {v:.4g} vs {l:g} ({m:.1f} %)" for n, v, l, m in THIN] or "none")
hard = [f for f in FAILS if f[3] == "error"]
sys.exit(1 if hard else 0)
