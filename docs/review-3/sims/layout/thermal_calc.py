#!/usr/bin/env python3
"""thermal_calc.py - class-D junction temperature at music power and at the governor limit (review-3 LAYOUT).

Loss model per chip (BTL): P = P_idle + sum_ch I_rms^2 * 2 * Rds(on,hot) + k_sw * sum_ch I_rms * PVDD
  P_idle  : TPA3118 PVDD*Icc (SLOS708G 6.5: 32 mA typ @24 V); TPA3220 0.55 W AD @30 V (SLASEN3B 7.6), scaled by PVDD/30
  Rds(on) : TPA3118 120 mOhm @25 C (6.5), TPA3220 70 mOhm LS and HS @25 C (7.5); x1.4 at Tj ~100 C
  k_sw    : switching loss per amp-volt, 2 ns effective edge x fsw (400 / 600 kHz) - a 2x-pessimistic guess
Thermal: Tj = T_box + P * theta.
  TPA3118 on Lite 2-layer: TI EVM 22 C/W (SLOS708G 6.4, 2-layer EVM); our 16-via EP, taken as 30 C/W; hwde's screen 48 C/W as the upper bound.
  TPA3220 on core 4-layer: 23.5 C/W JEDEC 2s2p (SLASEN3B 7.4); x1.1 for 0.5 oz inner (constraints note) and x1.15 for the second chip on the
  same board -> 30 C/W; 36 C/W as the upper bound.
  Box air: Lite 52 C (1/8 power) / 62 C (governed), round-1 THERMAL-6 (30 C room); core taken the same.
Limits: TPA3118 OTE 150 C (latched, firmware recovers); TPA3220 OTW 115 C min / 125 typ (SLASEN3B 7.5), OTE above that.
Exit 1 if any case passes its shutdown limit at the upper-bound theta, or passes OTW-min at the nominal theta.
"""
import math, sys

CASES = [
    # name, chip, PVDD, RL, per-channel average W list, box air C, theta nom, theta hi, shutdown C, warn C
    ("Lite 1/8 clip, 6 ohm (3.6 W/ch)", "3118", 20, 6, [3.6, 3.6], 52, 30, 48, 150, None),
    ("Lite governed 13 W/ch, 6 ohm", "3118", 20, 6, [13, 13], 62, 30, 48, 150, None),
    ("Lite governed 13 W/ch, 4 ohm", "3118", 20, 4, [13, 13], 62, 30, 48, 150, None),
    ("Plus 1/8 clip, 6 ohm, 1 ch/chip (7.4 W)", "3220", 28, 6, [7.4], 52, 30, 36, 150, 115),
    ("Plus governed 14.8 W/ch, 6 ohm, 1 ch/chip", "3220", 28, 6, [14.8], 62, 30, 36, 150, 115),
    ("Plus governed 9.6 W/ch, 4 ohm, 1 ch/chip", "3220", 28, 4, [9.6], 62, 30, 36, 150, 115),
    ("Ultra both zones 17 W total: 4.25 W/ch, 4 ohm, 2 ch/chip", "3220", 28, 4, [4.25, 4.25], 62, 30, 36, 150, 115),
    ("Ultra one zone 9 W: 4.5 W/ch, 4 ohm, 2 ch on one chip", "3220", 28, 4, [4.5, 4.5], 62, 30, 36, 150, 115),
]


def loss(chip, pvdd, rl, chans):
    if chip == "3118":
        idle, rds, fsw = pvdd * 0.032, 0.120 * 1.4, 400e3
    else:
        idle, rds, fsw = 0.55 * pvdd / 30, 0.070 * 1.4, 600e3
    p = idle
    for w in chans:
        i = math.sqrt(w / rl)
        p += i * i * 2 * rds + i * pvdd * 2e-9 * fsw
    return p


def main():
    bad = 0
    print(f"{'case':58s} {'P_chip':>7s} {'Tj nom':>7s} {'Tj hi':>7s}  limits")
    for name, chip, v, rl, ch, tbox, th, thh, sd, warn in CASES:
        p = loss(chip, v, rl, ch)
        tn, thi = tbox + p * th, tbox + p * thh
        f = ""
        if thi > sd or (warn and tn > warn):
            f = "FAIL"; bad += 1
        lim = f"OTE {sd}" + (f", OTW min {warn}" if warn else "")
        print(f"{name:58s} {p:6.2f}W {tn:6.0f}C {thi:6.0f}C  {lim} {f}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
