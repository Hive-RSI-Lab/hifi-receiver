#!/usr/bin/env python3
"""PVDD bulk caps: ripple current at the operating points that really occur (R6, check_thermal's C5/C6 174 %).

Model. BTL AD mode: the bridge draws +i or -i from PVDD every cycle, so its HF
current (above audio) is i*sqrt(1-(2D-1)^2) instantaneously; with a sine of
modulation m the channel's rms is Irms*sqrt(1-0.75*m^2) (full swing: 0.5*Irms,
thermal.md's figure; small signal: about Irms). Channels add in rms (the chips
aren't phase locked); `in phase` is the linear sum, the upper bound.
Split at 600 kHz by impedance: ceramics C207-210/C307-310 (4 x 1 uF, about half
left at 28 V, + 4 x 100 nF), C7 10 uF (about 3 uF at 28 V, 20 mm off), bulk caps
each ESR + ESL (10 nH) + 5 nH of path. All numbers are estimates, none measured.
"""
import math
W = 2 * math.pi * 600e3


def zc(c, esl=1.5e-9, esr=0.01):
    return complex(esr, W * esl - 1 / (W * c))


def par(zs):
    return 1 / sum(1 / z for z in zs)


def split(bulk):
    """Each bulk cap's share of the total HF current; bulk = [(esr, esl)]"""
    z_b = [complex(r, W * (l + 5e-9)) for r, l in bulk]
    z_c = par([zc(2e-6), zc(0.4e-6), zc(3e-6, esl=15e-9)])
    y = sum(1 / z for z in z_b) + 1 / z_c
    return [abs((1 / z) / y) for z in z_b]


def hf(irms, vpk, pvdd):
    m = min(vpk / pvdd, 1.0)
    return irms * math.sqrt(1 - 0.75 * m * m)


# name, W per active channel, ohm, active channels, PVDD
CASES = [("Plus governed 19 W, 4 ohm", 9.5, 4, 2, 28),
         ("Plus governed 19 W, 4 ohm, 20 V", 9.5, 4, 2, 20),
         ("Ultra governed 17 W, 4 ohm", 4.25, 4, 4, 28),
         ("Plus DJ direct full sine, 4 ohm (129 W in, 140 W EPR; guard trips in 5.8-24 s)", 55, 4, 2, 28),
         ("Plus DJ direct full sine, 4 ohm, 20 V (over contract: hands over in 20 ms)", 50, 4, 2, 20)]
PX = (0.10, 10e-9, 0.72, "Rubycon PX 470u/50V")
POLY = (0.018, 10e-9, 4.1, "polymer PA50V470M10x17")
BUILDS = {"3 x PX (as built)": [PX, PX, PX],
          "C5 PX, C6 + C311 polymer": [PX, POLY, POLY]}
if __name__ == "__main__":
    for bn, b in BUILDS.items():
        s = split([(x[0], x[1]) for x in b])
        print(f"\n{bn}: share of HF per cap", [round(x, 2) for x in s])
        for name, w, r, n, v in CASES:
            i = math.sqrt(w / r)
            h = hf(i, i * r * math.sqrt(2), v)
            tot, top = h * math.sqrt(n), h * n
            per = [x * tot for x in s]
            print(f"  {name}: channel {i:.2f} A rms, HF {tot:.2f} A rss ({top:.2f} in phase)")
            print("    per cap " + ", ".join(f"{p:.2f} A = {p / c[2]:.0%}" for p, c in zip(per, b))
                  + "  | in phase worst " + f"{max(x * top / c[2] for x, c in zip(s, b)):.0%}")
            print("    heat " + ", ".join(f"{p * p * c[0] * 1e3:.0f} mW" for p, c in zip(per, b)))
    # a 10 x 20 can: 127 K/W (thermal.md 5), ~4 J/K, so tau ~ 500 s
    print("\ncan: tau ~ 500 s, so 24 s of a 22 K steady rise is",
          round(22 * (1 - math.exp(-24 / 500)), 1), "K")
