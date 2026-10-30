"""Lite sub-out mute (Q12-Q15) off while playing: corners the round-2 benches don't cover.

Round 2's tb_play_cap uses a level-1 2N7002 (no subthreshold current, no tempco) at 25 C and a
10k sub. This bench uses models_ekv.inc (subthreshold, Vth -4 mV/K), the Q12 network at the same
temperature, Vth 1.0/1.5/2.1 V, 25/50/70 C, sub inputs 10k and 100k, and three levels: the
firmware cap (LOUT2 code 26, -6 dB, 0.71 Vpk), the init level the DAC_CONTROLSET writes for the
first >= 2 s (code 30, 0 dB, 1.41 Vpk), and DJ-direct (FLX4 2.1 Vrms -> 0.42 Vpk).
Measures: THD of SUB_L at 40 Hz, negative-peak loss against the mute-less line, and L->R
crosstalk at 1 kHz through MUTE_S (R driven by nothing). Exit 1 if any case at the firmware cap
or DJ level fails (THD > 0.1 % or crosstalk > -80 dB); the 0 dB init cases are reported only.
"""
import sys, json, itertools
import numpy as np
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import ngs
D = __file__.rsplit('/', 1)[0] + '/'

def net(vth, tc, rsub, ampl, ampr, f, mute=True):
    q = "" if mute else "*"
    return f""".title mute corner
.include {D}models_r3.inc
.include {D}models_ekv.inc
.temp {tc}
V5 p5 0 5
Vres res 0 4.4
R80 p5 det 120k
R81 det 0 22k
QDET mute det 0 Q3904
R82 res mute 47k
{q}X13 subl mute ms E7002 vth25={vth} tc={tc}
{q}X15 subr mute ms E7002 vth25={vth} tc={tc}
{q}X14 0 mute ms E7002 vth25={vth} tc={tc}
Vl lout 0 SIN(1.65 {ampl} {f})
Vr rout 0 SIN(1.65 {ampr} {f} 0 0 180)
C47 lout acl 10u
R36 acl subl 100
R37 subl 0 47k
RSL subl 0 {rsub}
C48 rout acr 10u
R38 acr subr 100
R39 subr 0 47k
RSR subr 0 {rsub}
.options reltol=1e-5 abstol=1e-13 vntol=1e-7
.tran {1/f/400} {2.0 if rsub > 50e3 else 1.0} 0 {1/f/400}
.end
"""

def wave(text, node, f, ncyc):
    v = ngs.vectors(text)
    t = np.array(v['time']); y = np.array(v[node.replace('v(', '').rstrip(')')])
    t0 = t[-1] - ncyc / f
    tu = np.linspace(t0, t[-1], ncyc * 400, endpoint=False)
    return np.interp(tu, t, y)

def thd(y, ncyc):
    s = np.abs(np.fft.rfft(y * np.hanning(len(y))))
    fund = s[ncyc - 1:ncyc + 2].max()
    harm = sum(s[k * ncyc - 1:k * ncyc + 2].max() ** 2 for k in range(2, 10))
    return float(np.sqrt(harm) / fund), float(fund)

levels = {"cap_-6dB": 0.71, "init_0dB": 1.41, "dj_flx4": 0.42}
rows, fails = [], []
for (lv, a), vth, tc, rsub in itertools.product(levels.items(), (1.0, 1.5, 2.1), (25, 50, 70), (10e3, 100e3)):
    y = wave(net(vth, tc, rsub, a, a, 40), 'v(subl)', 40, 10)
    y0 = wave(net(vth, tc, rsub, a, a, 40, mute=False), 'v(subl)', 40, 10)
    d, _ = thd(y, 10)
    loss = float(y.min() - y0.min())  # >0: the negative peak lost this much
    row = dict(level=lv, vth=vth, tc=tc, rsub=rsub, thd_pct=round(100 * d, 4), neg_peak=round(float(y.min()), 4),
               neg_peak_loss_mV=round(1e3 * loss, 2))
    if lv != "init_0dB" and (d > 1e-3):
        fails.append(row)
    rows.append(row)
# crosstalk, worst corner for MUTE_S: hottest, lowest Vth, 100k sub; R undriven
for lv, a in levels.items():
    for vth, tc in ((1.0, 70), (2.1, 25)):
        for rsub in (10e3, 100e3):
            txt = net(vth, tc, rsub, a, 0.0, 1000)
            yr = wave(txt.replace('.tran 0.0000025 1.0', '.tran 2.5e-6 1.0'), 'v(subr)', 1000, 20)
            yl = wave(txt, 'v(subl)', 1000, 20)
            xt = 20 * np.log10(max(np.ptp(yr - yr.mean()), 1e-12) / np.ptp(yl))
            row = dict(level=lv, vth=vth, tc=tc, rsub=rsub, xtalk_db=round(float(xt), 1))
            if lv != "init_0dB" and xt > -80:
                fails.append(row)
            rows.append(row)
for r in rows:
    print(json.dumps(r))
print("FAIL" if fails else "PASS", len(fails), "failing case(s)")
sys.exit(1 if fails else 0)
