# TPA3221 heatsink: options checked (2026-10-29)

Lead's order of preference: (1) a board-mount heatsink with solder pins that JLC fits in THT assembly; (2) a pad-down amp that meets 28 V / 77 W; (3) the enclosure's heat path.

**Need** (THERMAL-1): 3.5 W at 1/8 power into 4 ohm, 45 C box air, Tj below the 115 C warning: total junction-to-air <= ~20 C/W. RthJC(top) 1.1 + pad 0.9 leaves <= ~18 C/W for the heatsink, in still air, for a ~14 x 6 mm slug under a 19 x 12 mm footprint.

**Decision (task 261029-075354, issue #47): option 2. U15/U16 are now the pad-down TPA3220DDW.** Nothing is fitted by hand. The cost is a lower continuous-power ceiling, which the firmware governor enforces (below).

## (1) Board-mount, JLC-fitted: not possible

JLC assembly search, 2026-10-29 (`selectSmtComponentList`, keywords "Heat Sinks", "heatsink", "SMD heatsink", "heat sink pin", "TO-220 heatsink"):

- **What's stocked:** generic aluminium extrusions only. C4650 / C361579 15x10x20 U-type (3.9k / 1k), C4649 / C108928 / C116600 15.5x10.5x21-24 H-type, C4661 23.5x16x25 2-pin (6.7k), C4693 23x16x20 2-pin (6.8k), C32487 30x30x25 2-pin, C286198 20x20x10 and others.
- **The pinned ones are TO-220 parts.** The transistor bolts to a vertical face, and the pins only hold the extrusion upright. None has a base that sits flat on an SMD package top.
- **The "SMD heatsink" hits are customer library entries** (C99000xxxxx), all with 0 stock.
- **No interface step:** JLC's assembly has no process that lays a thermal pad or adhesive between a part's top and a heatsink. A heatsink on the TPA3221's top slug is a hand step whatever part is bought.
- **No room:** the 19 x 12 mm space over U15 is boxed in by the output inductors and D1 (thermal.md 1.5). Even the smallest pinned part, 15x10, would need holes where there is no room. Its 25-30 C/W is over the 18 C/W allowance anyway.

## (2) Pad-down amp: TPA3220, taken

The first check here said that the pad-down relatives stop at 26 V. That was wrong. TI's **TPA3220** (SLASEN3B, rev B Aug 2018) is the TPA3221 with its PowerPAD on the bottom. Its device-comparison table lists the TPA3221 as **pin-compatible**: pad on top, "Y". thermal.md 1.5 had already named it as the contingency.

| | TPA3221DDV (was) | TPA3220DDW (now) |
|---|---|---|
| Pin map | — | identical, pins 1-44 (SLASEN3B p3-4 vs SLASEE9C table 5-1) |
| Thermal pad | top, needs a heatsink | bottom, soldered to the GND pour (pad 45) |
| PVDD | 7-30 V rec., 37 V abs | 7-**32** V rec., 37 V abs |
| Gain / controller-peripheral table | 20k/100k ctl, 75k/47k per. at 24 dB | same (SLASEN3B table 2) |
| Output at 30 V, 4 ohm, 1 % | 88 W | 89 W (2 ch, 3 s) |
| Inverting | yes | yes |
| OTW / OTE | 115-135 / 145-165 C | 115-135 / 145-165 C |
| RthJA, JEDEC 2s2p | 44.8 C/W (no heatsink) | **23.5 C/W** (RthJC(bot) 1.1, RthJB 4.5) |
| LCSC / JLC stock | C475500, 1,026 | C2879306, **0** |
| Elsewhere | — | Mouser 6,566 ($3.74 at 100), DigiKey 2,032 (Octopart, 2026-10-29) |

**What changed on core:**
- **Symbol:** `aiee:TPA3220DDWR` is the TPA3221 symbol plus pin 45 (GND, EP).
- **Footprint:** `aiee:HTSSOP-44-1EP_L14.0-W6.1-P0.64-LS8.1-EP7.3x4.43`.
  - Pads 1-44 are the old land, unchanged in position.
  - The EP is 7.3 x 4.43 mm, per TI's DDW0044D land.
  - Paste is cut into 4 windows of 3.0 x 1.7 mm (63 %).
  - 15 thermal vias, 0.3 mm drill on 0.6 mm pads, sit at 1.5 x 1.3 mm pitch into In1's solid GND plane and the B.Cu GND pour.
- **Copper:** the footprint was swapped in place with pcbnew, so pads 1-44 kept their tracks. The tracks and vias that clashed with the EP or its vias were ripped: 15 signal nets, plus 7 GND stitching vias that the EP vias replace. They were re-routed with hwde's `route_finish`, which also moved AMP_IN_L and B_CMUTE, and their stubs were cleaned up. DRC is clean (0 violations, 0 unconnected, schematic parity 0).

**Thermal on this board** (the 4-layer EP land with 15 vias into In1 GND and B.Cu):
- **Thermal resistance:** TI's JEDEC 2s2p figure is 23.5 C/W (SLASEN3B 7.4), on 2 oz outer and 1 oz inner copper with the part alone. Core is 1 oz / 0.5 oz and shares its board with the buck and, on Ultra, zone B. I use **26 C/W**. hwde's check_thermal (branch 261029-050245) takes the datasheet's 23.5 as-is.
- **Chip loss** comes from TI's system-loss curve (SLASEE9C fig 6-9, read in thermal.md 1.2, 4 ohm, AD mode) less the output filter. Tj = box air + loss x 26 C/W:

| Case | Out (2 ch, 4 ohm) | Chip loss | Tj, 25 C box | Tj, 40 C box |
|---|---|---|---|---|
| Idle | 0 | 0.45 W | 37 C | 52 C |
| Loud home listening | 2 x 1 W | 0.8 W | 46 C | 61 C |
| Music at 1/8 power | 2 x 9.6 W | 3.5 W | **116 C** | **131 C** |
| TI's "2 x 60 W continuous" (with its EVM's copper) | 2 x 60 W | ~10 W | 284 C | 299 C |
| Supply-limited sine, 140 W in | ~113 W | ~19 W | 520 C | 535 C |

  - Into 8 ohm, 1/8 power is 1.5 W: 64-79 C. Pass.
  - **The 2 x 60 W figure isn't available here.** About 10 W of chip loss needs roughly 10 C/W junction-to-air, which core's copper doesn't give without a heatsink. Neither does the supply-limited sine.
  - **Music at 1/8 power into 4 ohm holds only with the governor's help.** Peaks are untouched: 77 W per channel for seconds, because the board's copper and FR4 take minutes to heat.
- **Highest continuous chip loss** (Tj 110 C, 5 K under OTW min) against box air:

| Box air | Chip loss | Out, 2 ch, 4 ohm | USB-C input, 60 s average |
|---|---|---|---|
| 25 C | 3.3 W | 2 x 8.8 W | 24 W |
| 40 C | 2.7 W | 2 x 6.7 W | 19.5 W |
| 45 C | 2.5 W | 2 x 6.1 W | 18 W |
| **60 C** (the Plus box as drawn, 30 C room) | **1.9 W** | **2 x 4.2 W** | **13 W** |

- **The box decides it.** check_thermal's enclosure model puts the Plus box's air at about 60-63 C at 30 C room with about 5 W inside. That box is 127 x 106 x 40 mm PETG, and only about 95 mm² of vent path is effective: the slots over and under U15 in series with the board's edge gaps. That is also 17 K under PETG's Tg, a warning.
  - Ultra, with both amps, reaches about 70 C: within 10 K of Tg.
  - If inlet, riser and outlet were each about 1500 mm², the Plus air would drop to about 45-49 C and the chip limit would rise to 2.4-2.8 W. That needs much bigger slots plus a clear riser past the board. It is a job for the enclosure task, and I've told its worker.

**Governor (firmware-requirements C2, now in `hifi_gov`):**
- **Plus:** the 60 s average speaker power is held to **8 W** of both channels (`thermal_out_w`). That is the chip's 1.9 W in the box as drawn, at 30 C room: about **13 W at the USB-C input** (0.47 A at 28 V).
- **Ultra:** **12 W** in total with both amps (`thermal_out2_w`), about 1.5 W per chip, about 20 W in.
- **Once the box gets the bigger vents:** these can rise to 11 W and 18 W.
- **Unchanged:** the 1 s supply budget (90 % of the contract), the OTW back-off (-3 dB every 2 s) and the FAULT/RESET retry.
- **Bring-up C3** measures the real board and box, and sets these limits from the measurement.

**Levers not taken:**
- **2 oz outer copper**, which TI recommends: it is about 10-15 % better, but core's fine-pitch routing was drawn to 1 oz rules.
- **HEAD modulation** (HEAD pin to AVDD): idle loss 0.55 → 0.38 W. That is small next to the conduction loss at 1/8 power, and it would change the modulation that the audio review signed off.

**Ordering:** LCSC has no TPA3220, so the order needs it bought into the JLC parts inventory first. JLC global sourcing, or a consigned reel from Mouser or DigiKey, does that. It is listed in `boards/core/fab/prebuy.csv`. This is the same step as any "thin" part in docs/bom-stock.md, and it costs about $2.10 more per amp than the TPA3221. The off-board heatsink and thermal pad (about $1.50 per amp) and their hand fitting go away.

## (3) Enclosure heat path: no longer needed

The lid-pressed heatsink is dropped. The Plus/Ultra lid and floor vents over and under the amps stay, because the board now sheds the amps' heat into the box air.
