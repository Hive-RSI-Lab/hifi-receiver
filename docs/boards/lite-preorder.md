# Lite pre-order fix round (2026-11-01)

What hwde 0600487's new checks found on lite-main, and what changed. All of it is
checks and simulation; no board has been built.

## Vias in pads (check_wicking)

15 pads had an open via in them. Now 0: nothing was left in a pad, the big pads included.
Scripts: `boards/lite-main/kicad/gen/wick_fix.py`.

| Pad | Change |
|---|---|
| U7.21 /TAP_R_C | via to (51.0, 93.393). The QFN's corner left no room, so /TAP_L_C's via moved too (to (50.2, 93.4)) and its F.Cu now runs along pin 22's row |
| Q12.1, Q13.2, Q13.3, Q14.2, C19.2 | via moved 0.7-0.9 mm, with stubs on both layers |
| U8.14 GND | via moved under the package, 1.3 mm. This is a 0.34 mm signal-row pin, not an exposed pad, so it was a must-move |
| D1.2 GND | via moved off the pad, 1.4 mm, 0.6 mm stub |
| C58.2, C59.2 GND | 3 vias each moved to a row beside the pad, 1.2 mm away, 0.6 mm stubs |
| C25.1, L5.2 +5V | via moved off the pad, to (86.3, 100.85) and (84.5, 94.0). They must stay: check_current wants 2 vias at each of these layer changes (0.8 A) |
| C30.2, R10.2 GND | in-pad via deleted |
| U5.39 (ESP32 exposed pad) | 2 vias nudged into the gaps between the 0.9 mm lands |

R10.2's via turned out to be the only tie between a 12 x 37 mm B.Cu GND island and the
main pour. Three stitch vias at (73.0, 95.5), (72.3, 103.2) and (68.4, 109.8) now tie it
to the main F.Cu pour.

## I2S crosstalk (check_si)

Fixed in `kicad/gen/i2s_respace.py` and in `firmware/components/hifi/esp/board_lite.c`.

- **Respacing on B.Cu:** BCLK x 50.6, DOUT x 52.1, LRCK x 53.6, so 1.5 mm with GND pour
  between them. They were 0.58-0.79 mm apart, 0.33 mm centre to centre in the diagonal at
  the ESP32 end (0.13 mm gap). The vias at both ends stayed.
- **Drivers declared:** `kicad/constraints.json` now has an `si` block saying U5 drives
  MCLK, BCLK, LRCK and DOUT and U7 drives DIN. Before, the sim tried both directions and
  kept the worst, so it reported noise "at the receiver" on the ESP32's own output pins.
- **Firmware:** BCLK, LRCK and DOUT go to GPIO_DRIVE_CAP_0 in `hifi_bsp_init`. Not
  compiled or run here: the host tests don't build board_lite.c.

| Pair (2 ns edge, no IBIS) | Before | After | Budget / margin |
|---|---|---|---|
| DOUT -> BCLK at U7.5 | 490 mV | below the top three, not reported | 165 / 396 mV |
| BCLK -> DOUT at U7.6 | 581 mV | below the top three, not reported | 165 / 396 mV |
| LRCK -> DOUT | 499 mV (at U5.11) | 344 mV at U7.6 | 165 / 396 mV |
| DOUT -> LRCK | not in the top three | 291 mV at U7.7 | 165 / 396 mV |

So the two remaining pairs are inside the noise margin and still over the 165 mV budget,
and check_si still warns on them. What-if with a slower edge, because the real CAP_0 edge
isn't known (assumed values, not measured): LRCK -> DOUT is 90 mV at t_r 4 ns, and 47 mV
at t_r 4 ns with a 100 ohm driver. Worth a scope on BCLK at bring-up.

## VBUS out of J1 (check_power_path)

`kicad/gen/vbus_neck.py`. Left stub 0.5 to 0.7 mm. Right stub 0.6 to 0.7 then 0.9 mm, and
its run to Q1's pour 1.0 to 1.7 mm. At 1.4 mm the check read 3.49 A at dT 10 C against 3 A
(2.17 A before), under its 1.25 x margin; at 1.7 mm it reads the path as pour only and gives
no number, so 3.49 A is the last rated figure, and it is above the 3.25 A a 65 W charger can give. The 0.3 mm via on the left branch is unchanged.

## Box: banana holes (R6-M2)

`enclosure/core_box.py` took the hole's x from the part's bbox centre, and a stray silk
line in the EasyEDA footprint inflates the bbox on one side. It now uses the part's own x.
All three boxes rebuilt, fit checks pass (Lite 44/44, Plus 46/46, Ultra 56/56). Measured
from the STLs: Lite's holes went from x 80.1, 91.1, 102.1, 113.1 to 80.4, 91.4, 102.4, 113.4
in the box frame, which is the sockets' own x + 3.0 (wall 2.4 + gap 0.6): offset 0.00 mm,
was 0.30 mm. Plus's and Ultra's holes also match their sockets (J20 18.413 against 18.4125).

## Also

- `architecture/constraints.json` deleted (constraints_drift). `kicad/constraints.json`
  already held everything in it, and `kicad/gen/power.py` no longer writes it.
- Regenerating fab on 0600487 moved J2/J3's CPL y from -75.523 to -74.730: bom_cpl now
  carries the model's offset (hwde 1c60f11). JLC places these jacks, so check them in its
  preview as usual; `cpl_verify` passes on the new file (offset 0.0 mm against the LCSC
  model for both). The zip also gains the IPC netlist (`lite-main.ipc`), and `prebuy.csv` plans 10 boards (it
  was 5), the build size used in the stock check.
