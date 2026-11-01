"""R5-PA: dissipation in the output Zobel resistors (4.7 R 0603, 0.1 W, with
470 nF X7R 0603 per leg; docs/boards/core-zobel.md) for the inputs the
firmware's tone guard (hifi_tone_check: >5 kHz sine capped at -16 dBFS) does
not see: music, and DJ direct (analog, no firmware in the path). Per leg the
Zobel sees half the BTL differential voltage. C at 470 nF and at 280 nF
(DC bias at the 14 V leg midpoint). Read-only calculation; exit 1 if a
case the person can reach without "force" passes 0.1 W."""
import math, sys
R, P_MAX = 4.7, 0.1
def p_leg(v_diff_rms, f, c):
    z = math.hypot(R, 1 / (2 * math.pi * f * c))
    return (v_diff_rms / 2 / z) ** 2 * R
cases = [
    # name, differential speaker Vrms in the band, band frequency, reachable without force
    ("tone guard limit: -16 dBFS 20 kHz (DAC 0 dBFS = 15.8 Vrms)", 15.8 * 10 ** (-16 / 20), 20e3, True),
    ("music at Plus thermal cap 9.6 W/ch 4 ohm, >5 kHz band -15 dB, at 10 kHz", math.sqrt(9.6 * 4) * 10 ** (-15 / 20), 10e3, True),
    ("loud DJ program 55 W/ch peak level, >5 kHz band -15 dB, at 10 kHz", math.sqrt(55 * 4) * 10 ** (-15 / 20), 10e3, True),
    ("DJ direct, FLX4 full-level 15 kHz test tone (14.8 Vrms)", 14.8, 15e3, True),
    ("DJ direct, FLX4 full-level 10 kHz test tone (14.8 Vrms)", 14.8, 10e3, True),
]
bad = 0
for name, v, f, reach in cases:
    p1, p2 = p_leg(v, f, 470e-9), p_leg(v, f, 280e-9)
    flag = "OVER" if max(p1, p2) > P_MAX else "ok  "
    print(f"{flag} {name}: {1e3*p2:.0f}-{1e3*p1:.0f} mW per 4.7R (0.1 W rating)")
    if reach and max(p1, p2) > P_MAX:
        bad = 1
sys.exit(bad)
