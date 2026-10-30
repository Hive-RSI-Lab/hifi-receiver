"""Lite sub-out mute at power loss: the round-2 bench (tb_powerloss) at 25 C with a level-1 FET,
re-run across temperature (Q12's release point moves -13 mV/K on +5V), Vth 1.0/2.5 V (2.5 = the
weakest mute), +5V fall 20 ms and 2 ms, sub 10k/100k, and the codec output collapsing when +5V
crosses 3.4 V (round 2's assumption: LP5907 dropout) or 3.0 V. Measures the worst SUB_L excursion.
Fails (exit 1) when |SUB_L| exceeds 0.2 V (round 2's bound) in any case.
"""
import sys, json, itertools
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import ngs
D = __file__.rsplit('/', 1)[0] + '/'

def net(tc, vth, tf, vcol, rsub):
    tcross = 2 + tf * (5 - vcol) / 5  # +5V falls linearly 5 -> 0 in tf from t = 2 s
    return f""".title powerloss
.include {D}models_r3.inc
.include {D}models_ekv.inc
.temp {tc}
V5 p5 0 PWL(0 0 0.01 5 2 5 {2+tf} 0)
QD p5 p5 res Q3904
C90 res 0 22u
R80 p5 det 120k
R81 det 0 22k
QDET mute det 0 Q3904
R82 res mute 47k
X13 subl mute ms E7002 vth25={vth} tc={tc}
X15 subr mute ms E7002 vth25={vth} tc={tc}
X14 0 mute ms E7002 vth25={vth} tc={tc}
Vl lout 0 PWL(0 0 1.0 1.65 {tcross} 1.65 {tcross+0.002} 0)
Vr rout 0 PWL(0 0 1.0 1.65 {tcross} 1.65 {tcross+0.002} 0)
C47 lout acl 10u
R36 acl subl 100
R37 subl 0 47k
RSL subl 0 {rsub}
C48 rout acr 10u
R38 acr subr 100
R39 subr 0 47k
RSR subr 0 {rsub}
.tran 20u 3 0 20u
.measure tran sub_min MIN v(subl) FROM=1.99 TO=3
.measure tran mute_at_col FIND v(mute) AT={tcross}
.measure tran p5_mute3v WHEN v(mute)=3 CROSS=LAST
.end
"""
fails = []
for tc, vth, tf, vcol, rsub in itertools.product((25, 50, 70), (1.0, 2.5), (0.02, 0.002), (3.4, 3.0), (10e3, 100e3)):
    m = ngs.meas(net(tc, vth, tf, vcol, rsub))
    row = dict(tc=tc, vth=vth, fall_ms=tf * 1e3, collapse_at_V=vcol, rsub=rsub,
               sub_min=round(m.get('sub_min', float('nan')), 3), mute_at_collapse=round(m.get('mute_at_col', float('nan')), 2))
    print(json.dumps(row))
    if not abs(row['sub_min']) <= 0.2:
        fails.append(row)
print("FAIL" if fails else "PASS", len(fails), "failing case(s)")
sys.exit(1 if fails else 0)
