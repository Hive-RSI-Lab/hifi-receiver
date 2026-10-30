"""Lite sub-out RCA (J3 tip, SUB_L) discharge into the new mute (Q13-Q15, Q12), round 2's fix.

Sources: IEC 61000-4-2 contact (150 pF / 330 ohm, +-2 kV and +-8 kV) and a Class-II
(Y-capacitor) ground: 2.2 nF charged to +-325 V (230 V mains peak) through 10 ohm, the
event when the RCA's centre pin meets before its sleeve with the charger or sub on a Y-cap.
Model: 2N7002 as models_ekv.inc plus a 75 V D-S avalanche clamp (BVdss 60 V min);
MMBT3904 Q12 with a 7 V B-E reverse breakdown (VEBO 6 V min); 10 nH on the SUB_MUTE trace
(14 mm, measured). Measures max |Vgs| of Q13/Q14/Q15 (abs max 20 V) and the peak current
into the codec side (R36). Exit 1 if any |Vgs| > 20 V.
"""
import sys, json
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import ngs
D = __file__.rsplit('/', 1)[0] + '/'

def net(kind, v, tvs=False):
    if kind == "iec":
        src = f"Cs hv 0 150p IC={v}\nRs hv tip 330\nSW1 tip subl 1 0 SWM\nV1 1 0 PWL(0 0 1n 1)\n.model SWM SW(VT=0.5 RON=0.1 ROFF=1e12)"
    else:
        src = f"Cs hv 0 2.2n IC={v}\nRs hv tip 10\nLs tip tip2 100n\nSW1 tip2 subl 1 0 SWM\nV1 1 0 PWL(0 0 1n 1)\n.model SWM SW(VT=0.5 RON=0.1 ROFF=1e12)"
    t = "Dt1 subl 0 DTVS\nDt2 0 subl DTVS\n.model DTVS D(IS=1e-14 BV=6 IBV=1m RS=0.3 CJO=10p)" if tvs else ""
    return f""".title esd
.include {D}models_r3.inc
.include {D}models_ekv.inc
{src}
{t}
V5 p5 0 5
Vres res 0 4.4
R80 p5 det 120k
R81 det 0 22k
QDET c12 det 0 Q3904
DBE12 0 det DZBE
.model DZBE D(IS=1e-14 BV=7 IBV=1m RS=2)
Lg c12 mute 10n
R82 res mute 47k
X13 subl mute ms E7002 vth25=2.0
X15 subr mute ms E7002 vth25=2.0
X14 0 mute ms E7002 vth25=2.0
DA13 ms subl DAV
DA15 ms subr DAV
DA14 ms 0 DAV
.model DAV D(IS=1e-14 BV=75 IBV=1m RS=1)
* the codec side: C47 10 uF (+ESL), the ES8388 pin diodes to +3V3A/GND
R36 subl acl 100
C47 acl raw 10u
Dp raw p33 DPIN
Dn 0 raw DPIN
.model DPIN D(IS=1e-12 RS=2)
V33 p33 0 3.3
Vraw_s raw rawm 0
Rraw rawm 0 1e9
R37 subl 0 47k
R38 subr acr 100
C48 acr 0 10u
R39 subr 0 47k
.tran 0.1n 300n 0 0.1n UIC
.measure tran vgs13max MAX par('v(mute)-v(ms)')
.measure tran vgs13min MIN par('v(mute)-v(ms)')
.measure tran vgd15min MIN par('v(mute)-v(subr)')
.measure tran vsubl_min MIN v(subl)
.measure tran vsubl_max MAX v(subl)
.measure tran vmute_min MIN v(mute)
.measure tran vmute_max MAX v(mute)
.measure tran ir36 MAX par('abs((v(subl)-v(acl))/100)')
.end
"""
fail = 0
for kind, vs in (("iec", (2000, -2000, 8000, -8000)), ("ycap", (325, -325))):
    for v in vs:
        for tvs in (False, True):
            m = ngs.meas(net(kind, v, tvs))
            g = max(abs(m["vgs13max"]), abs(m["vgs13min"]))
            print(json.dumps(dict(src=kind, v=v, tvs=tvs, vgs_max=round(m["vgs13max"], 1), vgs_min=round(m["vgs13min"], 1),
                                  sub_l=[round(m["vsubl_min"], 1), round(m["vsubl_max"], 1)],
                                  gate=[round(m["vmute_min"], 1), round(m["vmute_max"], 1)], codec_side_A=round(m["ir36"], 2))))
            if not tvs and g > 20:
                fail += 1
print("FAIL" if fail else "PASS", fail, "case(s) with |Vgs| > 20 V (as built)")
sys.exit(1 if fail else 0)
