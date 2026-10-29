# TPA3221 heatsink: options checked (2026-10-29)

Lead's order of preference: (1) a board-mount heatsink with solder pins that JLC fits in THT assembly; (2) a pad-down amp that meets 28 V / 77 W; (3) the enclosure's heat path.

**Need** (THERMAL-1): 3.5 W at 1/8 power into 4 ohm, 45 C box air, Tj below the 115 C warning: total junction-to-air <= ~20 C/W. RthJC(top) 1.1 + pad 0.9 leaves <= ~18 C/W for the heatsink, in still air, for a ~14 x 6 mm slug under a 19 x 12 mm footprint.

**(1) Board-mount, JLC-fitted.** LCSC search (hwde parts_search, 2026-10-29) finds only generic aluminium extrusions: C4650 / C361579 15x10x20 U-type (4.5k / 1k stock), C286198 20x20x10, C286200 20x20x15.3, C4708 30x25x25. None is listed with solder pins for a pad-up SMD IC, none states a thermal resistance, and the ones that fit the 19x12 mm area (15x10x20) are rated about 25-30 C/W natural convection by their class: over the 18 C/W allowance (Tj ~ 45 + 3.5 x (2 + 28) = 150 C). **Not verified, and the calculation says it fails.** No part is selected.

**(2) Pad-down amp at 28 V / 77 W per channel.** The TPA3221 is TI's part for this spec; its pad-down relatives (TPA3116/3118) stop at 26 V / ~50 W. Not found; would be a new design.

**(3) Enclosure heat path** (the current design): heatsink pressed from the lid with a 0.2 mm 3 W/mK pad, <= 20 C/W (a 25 mm-tall finned part, which the 19 x 12 mm footprint only just allows), plus the firmware governor. Still not a person-free step if the heatsink is hand-fitted; it needs to be part of the enclosure kit or a JLC/assembler-fitted item.

**Status:** option 3 stays in the design as the only one that holds the margin on paper; options 1 and 2 are open. A follow-up brief asks for a purchasable, assembly-fittable heatsink spec or an amp change. Until then the Plus/Ultra order has a hand-fitted heatsink, which contradicts the 'plug in and leave it' flow: the known-unknowns list keeps it as an open item for Plus/Ultra.
