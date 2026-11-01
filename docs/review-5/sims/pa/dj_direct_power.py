"""R5-PA: core in DJ direct (TS5A23157 puts the RCA line in straight on the
TPA3220s: no DAC, no volume, no governor) with a DJ deck at full line level,
against the USB-C contract. Read-only calculation from the board's values.

Chain (boards/core/kicad/gen/blocks_audio.py, blocks_amp.py):
  FLX4 master 2.1 Vrms (review-3 audio.md: rated 2.0-2.1 Vrms, unverified)
  -> R100/R101 4.7k/4.7k (Thevenin 0.5x, 2.35k) -> C100 4.7 uF
  -> load R102 100k (to VREF) || TPA3220 RIN 24 kOhm +-20 % at 24 dB
     (SLASEN3B 9.3.5 table 3; per pin, via the 10 uF input caps)
  -> TS5A23157 (~10 ohm) -> TPA3220 24 dB (15.85x, SLASEN3B table 2), IN_M AC-grounded.
Clip: 0.95 x VBUS peak (governor's swing). Efficiency 0.88, idle 4 W (hifi_gov_cfg_core).
Plus: L on U15, R on U16 (2 loaded channels). Ultra with zone B on: 4 loaded channels.

The firmware's DJ-direct guard (hifi_line.c hifi_djg_*) only compares a
60 s average of input power against the amps' THERMAL budget (Plus 19 W out
-> 25.6 W in) and OTW; nothing limits the input on the charger's time scale
(ms), which the governed path does with peak_frac 1.0 per block.

For each case: the mean input over a bass half-cycle-scale block for a 0 dB
low-frequency passage (a sustained sine at the deck's full level), and the
time the guard's 60 s EMA takes to trip. Exit 1 if any case the person can
reach (Plus 4/6 ohm, Ultra 4/6 ohm on 45-140 W PD chargers) exceeds 1.0x the
contract with no fast limit."""
import math, sys

V_DECK = 2.1
TH_GAIN, TH_R = 0.5, 2350.0
R_BIAS, R_IN = 100e3, 24e3
GAIN = 10 ** (24 / 20)
SWING, EFF, IDLE = 0.95, 0.88, 4.0
GUARD_LIMIT_PLUS = 19.0 / EFF + IDLE   # hifi_djg_limit_w with thermal_out_w 19, amps 1
GUARD_LIMIT_ULTRA2 = 17.0 / EFF + IDLE  # thermal_out2_w 17 with two amps
TAU = 60.0

def v_amp_in(rin):
    zl = 1 / (1 / R_BIAS + 1 / rin)
    return V_DECK * TH_GAIN * zl / (zl + TH_R)

contracts = [("EPR 28 V 5 A (140 W)", 28.0, 5.0), ("SPR 20 V 5 A (100 W)", 20.0, 5.0),
             ("SPR 20 V 3.25 A (65 W)", 20.0, 3.25), ("SPR 20 V 2.25 A (45 W)", 20.0, 2.25)]
bad = 0
for rin in (R_IN * 0.8, R_IN, R_IN * 1.2):
    print(f"RIN {rin/1e3:.1f}k: amp input {v_amp_in(rin):.3f} Vrms, speaker {v_amp_in(rin)*GAIN:.2f} Vrms "
          f"({v_amp_in(rin)*GAIN*math.sqrt(2):.1f} Vpk)")
vspk = v_amp_in(R_IN) * GAIN
print()
for model, nch, guard in (("Plus", 2, GUARD_LIMIT_PLUS), ("Ultra 2 zones", 4, GUARD_LIMIT_ULTRA2)):
    for rl in (4.0, 6.0):
        for name, v, i in contracts:
            vpk = min(vspk * math.sqrt(2), SWING * v)
            p_ch = vpk ** 2 / (2 * rl)                   # sine, at most the clip sine
            if vspk * math.sqrt(2) > SWING * v:          # clipped: toward a square wave
                k = vspk * math.sqrt(2) / (SWING * v)
                p_ch *= min(2.0, 1 + 0.6 * (k - 1))     # rough: +60 % per unit overdrive, capped at square
            p_in = nch * p_ch / EFF + IDLE
            ratio = p_in / (v * i)
            t_trip = -TAU * math.log(1 - guard / p_in) if p_in > guard else float('inf')
            flag = "OVER" if ratio > 1.0 else "ok  "
            if ratio > 1.0:
                bad = 1
            print(f"{flag} {model:13s} {rl:.0f} ohm {name:24s} speaker {min(vspk, SWING*v/math.sqrt(2)):5.1f} Vrms "
                  f"in {p_in:6.1f} W = {ratio:4.2f}x contract ({p_in/v:4.1f} A); guard trips after {t_trip:4.1f} s")
print(f"\nguard limits: Plus {GUARD_LIMIT_PLUS:.1f} W, Ultra 2 zones {GUARD_LIMIT_ULTRA2:.1f} W (60 s EMA)")
sys.exit(bad)
