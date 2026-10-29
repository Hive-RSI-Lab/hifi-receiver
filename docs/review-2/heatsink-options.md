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

**Thermal on this board:**
- **Thermal resistance:** JEDEC's 23.5 C/W assumes 2 oz outer and 1 oz inner copper with the part alone. Core is 1 oz / 0.5 oz, sits in a box and shares its board with the other amp and the buck. I use **26 C/W (23.5-30)**. That is the same +0-25 % thermal.md applied to the TPA3221's no-heatsink figure.
- **1/8 power, 8 ohm (1.5 W):** Tj = 45 + 1.5 x 26 = **84 C**. That is 31 K under OTW min. Pass.
- **1/8 power, 4 ohm (3.5 W):** Tj = 45 + 3.5 x 26 = **136 C** (116-180). OTW asserts. **The pad-down part alone does not hold this case**, just as the stick-on heatsink didn't (118-139 C, thermal.md 1.4). Only the clamped, lid-vented heatsink did on paper (101-108 C), and that one is fitted by hand.
- **Highest continuous chip loss under OTW min - 5 K, at 45 C box air:** (110 - 45) / 26 = **2.5 W**. That is about 2 x 4.5 W continuous average into 4 ohm, or about 2 x 10 W into 8 ohm.
  - Music peaks are not limited: 77 W per channel stays available for seconds. The limit is on the 60 s average.
  - At 35 C box air the ceiling is 2.9 W.

**Governor (thermal.md 1.6, THERMAL-2), new limits for the pad-down part:**
- **Long term (60 s average):** ≤ **16 W in** at the USB-C input, about 0.57 A at 28 V. Before, with the heatsink, it was 30 W. The 16 W is about 9 W out, 3.3 W amp loss and about 2 W for the rest of the board.
- **Ultra:** two chips share the box. The per-chip limit stays, so the total is ≤ 28 W in.
- **Unchanged:** the short-term limit (5 s, ≤ 70 W in), the OTW back-off (-3 dB every 2 s) and the FAULT/RESET retry.
- **Board time constant:** much longer than the heatsink's 130 s, because the heat sits in the board's copper and FR4. The 60 s window stays safe.

**Levers not taken:**
- **2 oz outer copper**, which TI recommends: it is about 10-15 % better, but core's fine-pitch routing was drawn to 1 oz rules.
- **HEAD modulation** (HEAD pin to AVDD): idle loss 0.55 → 0.38 W. That is small next to the conduction loss at 1/8 power, and it would change the modulation that the audio review signed off.

**Ordering:** LCSC has no TPA3220, so the order needs it bought into the JLC parts inventory first. JLC global sourcing, or a consigned reel from Mouser or DigiKey, does that. It is listed in `boards/core/fab/prebuy.csv`. This is the same step as any "thin" part in docs/bom-stock.md, and it costs about $2.10 more per amp than the TPA3221. The off-board heatsink and thermal pad (about $1.50 per amp) and their hand fitting go away.

## (3) Enclosure heat path: no longer needed

The lid-pressed heatsink is dropped. The Plus/Ultra lid and floor vents over and under the amps stay, because the board now sheds the amps' heat into the box air.
