# core output Zobels (issue #137)

**Finding.** core's TPA3220 output filter is 10 uH per leg plus 1 uF to GND.
With no speaker plugged in, nothing damps it but the inductor's 30 mOhm DCR, so
it peaks about +40 dB near 50 kHz (hive-hwde's sim_board class-D review; review-2
AUDIO-12). Each leg already had a "snubber", 4.7 R + 1 nF to GND, but 1 nF is
3.2 kOhm at 50 kHz, so it damped nothing.

**Change.** The snubber caps become Zobel caps: C228, C231, C234, C237 (zone A)
and C326, C329, C332, C335 (zone B) go from 1 nF to **470 nF/50 V X7R 0603**,
Yageo CC0603KRX7R9BB474, LCSC C513577 (extended, 1.3 M in stock, about $0.02).
The 4.7 R 0603 resistors stay. Same footprints, so `board_update` changed only the
value and LCSC fields on the routed board (`reports/board-update-zobel.json`):
no copper moved.

**Sim** (`docs/boards/sims/zobel_sweep.py`, ngspice, one leg):

| Zobel per leg | open load peak | 4 ohm | 6 ohm | 8 ohm | Rz at full-swing 20 kHz sine |
|---|---|---|---|---|---|
| 4.7 R + 1 nF (before) | +39.7 dB @ 50 kHz | -0.6 dB @ 20 kHz | +0.4 | +0.9 | ~0 |
| **4.7 R + 470 nF (now)** | **+15.8 dB @ 43 kHz** | -0.2 | +0.9 | +1.4 | 1.7 W |
| 4.7 R + 280 nF (470 nF after DC-bias loss at 14 V) | +21.5 dB | -0.3 | +0.8 | +1.2 | ~1 W |
| 4.7 R + 1 uF | +9.4 dB | -0.2 | +1.0 | +1.6 | 6.2 W |

Loaded responses stay within the speaker's own tolerance, and the 6 ohm
Vienna Acoustics case moves by about half a dB.

**Power.** The resistor is a 0.1 W 0603. With music it carries milliwatts: the
energy above 10 kHz is 20 dB or more below the total. A steady full-scale
sine above a few kHz would overload it. So the firmware's test tones refuse a
sine above 5 kHz louder than -16 dBFS without `"force": true` (about 43 mW at
20 kHz; `hifi_tone_check`, docs/test-api.md). Sweeps pass through the top band in
milliseconds and stay as they were. If a resistor did open, the board would be
back to the undamped filter it had before this change, not a fault.

**Not done.** A 1206 or 2512 resistor (and 1 uF) would damp more and take a
full-power sine. The row of parts is at 1.7 mm pitch, though, so that is a
placement and routing ECO. That's left for a later touch if the bench shows
ringing with the speakers off.

## Lite, checked the same way

Lite's TPA3118 filter is the same: 10 uH plus 1 uF per leg, at 20 V PVDD. So it
has the same +40 dB open-load peak, and it has no Zobel or snubber parts at all
(review-3 R3-AMP-5 left them off, as the common TPA3118 boards do). Adding them
means 8 new parts placed and routed on an order-ready board. The risk is only
with no speaker connected, from ultrasonic content in the signal. The ES8388's
DAC filters its images, and the amp's own over-current limit stays in place.
So Lite is left as it is. The check is a capture with the speakers off at
bring-up, which review-2 AUDIO-12 asks for and `bench/` doesn't run yet. If it
shows ringing, add footprints on the next spin.
