"""Charger peak current under the firmware governor (hifi_gov.c, budget_frac 0.9 of the contract's
mean power, 1 s window). A sustained low-crest signal (test tone, sweep, sub-bass) at the budget
draws sin^2 power from the supply: peak = idle + 2*(mean - idle). The PVDD bulk takes only the part
its impedance at 2f allows against the cable+source (0.15 ohm). A PD source guarantees its PDO
current; above it (Peak Current bits 00) it may OCP/hard-reset. Exit 1 when the charger peak
exceeds 1.1x the contract current for any case. Pure calculation, re-runnable."""
import math, sys
CASES = [  # board, contract V, A, bulk F, idle W, amp eff, max out W (clip, both ch, 4 ohm sine)
    ("lite 60 W (20 V 3 A)", 20, 3.0, 690e-6, 2.5, 0.88, 2 * (0.953 * 20) ** 2 / 8),
    ("lite 45 W (20 V 2.25 A)", 20, 2.25, 690e-6, 2.5, 0.88, 2 * (0.953 * 20) ** 2 / 8),
    ("core 45 W (20 V 2.25 A)", 20, 2.25, 1410e-6, 4.0, 0.88, 2 * (0.95 * 20) ** 2 / 8),
    ("core 65 W (20 V 3.25 A)", 20, 3.25, 1410e-6, 4.0, 0.88, 2 * (0.95 * 20) ** 2 / 8),
    ("core 100 W (20 V 5 A)", 20, 5.0, 1410e-6, 4.0, 0.88, 2 * (0.95 * 20) ** 2 / 8),
    ("core 140 W (28 V 5 A)", 28, 5.0, 1410e-6, 4.0, 0.88, 2 * (0.95 * 28) ** 2 / 8),
]
ZS = 0.15
fail = False
print(f"{'case':26s} {'f_tone':>6s} {'Pin_mean':>8s} {'Pin_pk':>7s} {'I_pk_chg':>8s} {'x I_pdo':>7s}")
for name, v, i, c, idle, eff, pclip in CASES:
    w = v * i
    pout = min((0.9 * w - idle) * eff, pclip)          # governor mean limit, capped at clipping
    pin_mean = idle + pout / eff
    for f in (40, 100):
        zc = 1 / (2 * math.pi * 2 * f * c)
        share = zc / math.hypot(zc, ZS)                 # fraction of the 2f ripple the source carries
        iac_pk = (pin_mean - idle) / v                  # sin^2: ripple amplitude = mean amp current
        ipk = pin_mean / v + share * iac_pk
        r = ipk / i
        fail |= r > 1.1
        print(f"{name:26s} {f:5d}Hz {pin_mean:7.1f}W {idle + 2 * (pin_mean - idle):6.1f}W {ipk:7.2f}A {r:6.2f}x")
print("FAIL: governor lets sustained tones draw peaks well over the PDO current" if fail else "PASS")
sys.exit(1 if fail else 0)
