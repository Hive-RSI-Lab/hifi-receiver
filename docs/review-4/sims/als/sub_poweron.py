"""Lite sub out at cold start: the ES8388 VMID ramp (0 -> 1.65 V, tau 0.25 s: C39 10 uF on the
codec's 50k VMID divider, reg 0x00 = 0x05 in DAC_CONTROLSET) reaches SUB_L through C47 10 uF into
R37 47k || the sub's input; the power-fail mute (Q13-Q15) has long released by then (squeezelite
starts the codec seconds after +5V is up). Prints the peak at the RCA and after a sub's own
2nd-order 20 Hz high-pass (Butterworth), against round 2's power-loss thump (1.61 V) and its
bound 0.14 V. Exit 1 if the high-passed peak exceeds 0.14 V.
"""
import sys
import numpy as np
from scipy import signal
fs = 20000.0
t = np.arange(0, 3.0, 1 / fs)
worst = 0
for tau in (0.25, 0.125, 0.025):  # 50k each leg / 50k total (VMIDSEL=01, as set); 5k (VMIDSEL=11): info only
    vmid = 1.65 * (1 - np.exp(-t / tau))
    for rsub in (10e3, 47e3, 100e3):
        r = 1 / (1 / 47e3 + 1 / rsub) + 100
        b, a = signal.bilinear([r * 10e-6, 0], [r * 10e-6, 1], fs)
        sub = signal.lfilter(b, a, vmid)
        bh, ah = signal.butter(2, 20, 'high', fs=fs)
        heard = signal.lfilter(bh, ah, sub)
        print(f"tau {tau*1e3:.0f} ms, sub {rsub/1e3:.0f}k: RCA peak {sub.max():.3f} V, after 20 Hz HPF {np.abs(heard).max():.3f} V")
        if tau > 0.1:
            worst = max(worst, np.abs(heard).max())
print("FAIL" if worst > 0.14 else "PASS", f"worst high-passed {worst:.3f} V vs 0.14 V")
sys.exit(1 if worst > 0.14 else 0)
