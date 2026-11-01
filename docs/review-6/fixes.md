# Review 6 fix round: core (Plus/Ultra), before ordering

Copper changes: `boards/core/kicad/gen/preorder_eco.py` (run with `LIN_GAP=0.6 MR_LAYERS=F.Cu,B.Cu MR_VIA_D=0.45 MR_VIA_H=0.2 MR_NOFILL=1`), then a refill. None of this is measured on hardware.

## Via in pad: filled and capped vias (POFV)

- The board's setup now says `(filling yes) (capping yes)`. `check_wicking` reads it: 910 of 910 vias filled, 0 findings (was 107). KiCad keeps the setting through a save.
- **JLC's limits:** 0.2-0.5 mm hole, annular ring at least 0.05 mm, more than 0.45 mm from other holes. All vias fit: holes are 0.2 and 0.3 mm, the ring at least 0.1 mm, and the smallest gap to a pad hole is 0.55 mm (a GND via beside H4).
- **J1's trap:** JLC's help page lets you ask for all vias of a given diameter, and its pages don't say what the quote-page option alone does to small plated pin holes. J1 has 16 of them at 0.4 mm. They must stay open, so ORDERING.md names the diameters in the order remark. U15/U16's 30 thermal-pad holes (0.3 mm) are footprint pads, not board vias, so `check_wicking` and `check_ep_paste` never count them. The remark covers them.
- **Price:** the table's $17.33 is for 5 boards at 91 x 70 mm, and $17.57 at 91 x 91 mm. It grows with area, and hwde's own quote says live prices ran 1.9-3.1x above its table. Treat it as a floor and read the quote page.
- **TPA3220 paste (63 %, 4 windows):** kept. 8 of each pad's 15 holes sit under a window. Filled, nothing wicks and 63 % is inside the usual 50-80 %; left open, it is the design as it was. It is fine either way. U9/U10/U13's windows don't change.
- **Tool gaps, not fixed (hwde tool work is held):** `check_ep_paste` reads filling per via and ignores the board's setting, which doesn't matter here as no via sits under EP paste. `order_submit.py` hard-codes `viaCovering: 1` (tented), so an order sent through it would drop POFV.

## R6-m3: LIN_L beside I2S_DOUT_MAIN

Moved LIN_L, not DOUT: the maze found no way out for DOUT at 0.7 or 1.0 mm from LIN_L/LIN_R. LIN_L now stays on B.Cu further north and west, then crosses LIN_R on a short F.Cu hop from (111.66, 144.05) to (114.56, 146.25). That adds 2 vias and makes it 77.9 mm long (was 69.8 mm).

| | before | after |
|---|---|---|
| I2S_DOUT_MAIN -> LIN_L, simulated at J5.1 | 110 mV | no coupled run left: 2.34 mm at the closest on the same layer |
| Worst I2S aggressor on LIN_L, simulated | 110 mV (DOUT) | 2.96 mV (LRCK, 0.85 mm at a 0.4 mm gap), budget 33 mV |

The brief asked for 1 mm. From DOUT it is 2.34 mm. The maze found no route with 1.0 or 0.8 mm to every I2S track, so the new run keeps 0.6 mm from them. Tighter gaps are in the part left as it was: LRCK 0.4 mm on the x=115.06 column (0.85 mm coupled, the 2.96 mV above) and I2C_SCL 0.39 mm at the via by R100. LIN_R comes within 0.14 mm where the hop returns to B.Cu.

## R6-m2: GND pour at the socket rings

`kicad/gen/extra_rules.dru` adds 0.3 mm round J20-J27's pads. `rebuild_board.sh` appends it after rules_gen. Measured on the saved fills: at least 0.32 mm, both faces (was about 0.1 mm).

## R6-m4: C229's path to L2

A 0.45/0.2 via on C229's existing F.Cu track at (48.61, 129.95) and a 0.6 mm B.Cu stub into the A L+ column. The path is about 21 mm (was about 70 mm).

## C5/C6: 174 % is not a real operating point; no part change

`check_thermal`'s 1.25 A came from a full-power sine shared by two caps. Plus and Ultra both fit three (C5, C6, C311), and the governor stops full power lasting. `c5c6_ripple.py` (estimates, ESR and ESL assumed; thermal:29 stays unverified):

| Case | Per cap, of the 0.72 A rating | If the channels are in phase |
|---|---|---|
| Plus governed, 19 W into 4 ohm | 0.45 A, 62 % | 88 % |
| Ultra governed, 17 W into 4 ohm | 0.43 A, 60 % | 120 % |
| Plus DJ direct, full sine, 140 W EPR, 4 ohm | 0.85 A, 118 % | 167 % |

- The DJ-direct line lasts 5.8-24 s before the guard trips. A can's time constant is about 500 s, so that is about 1 K of rise. On a smaller charger it leaves within 20 ms (R5-PA-2).
- Life at the governed point: `check_thermal` gives 69,900 h at 53.7 C (its target is 30,000 h), playing at the governor's limit all the time.
- Bass at 2f isn't in the script. By hand it adds little if the charger supplies most of it, as it should at 100 Hz; the rating there is 0.6 A.
- A polymer on C6 and C311 (JIERR PA50V470M10x17, C46550425, about $0.36) was looked at and rejected: it moves the current, but the remaining C5 goes up to 92 % at the governed point, so all three would have to change. That buys nothing the numbers ask for.

`constraints.json` now carries these figures and lists C311. `check_thermal` still warns at 118 %: true, and only the guard keeps it short.

## Not done

- R5-DAL-10 (100 pF at D8 on LIN_L/LIN_R): new parts, and the crosstalk it would help with is gone.
- R5-DAL-1 (C316-C319 closer to U16): not near this work, and it needs a re-place.
- Also removed: the stale `fab/ultra/core-ultra_BOM_*.csv` (R6-m8).

## Fab regenerated on hwde 0600487

- **J4/J5 in the CPL moved 0.79 mm in y (Plus and Ultra).** No part moved. Main's CPL came from an older hwde; the newer `bom_cpl` carries the part model's offset, which clears main's 2 `cpl_offset` warnings on them. Check J4/J5 in JLC's preview, as for every part.
- **dfm:** passes, with the same 4 waived and 0 failing. 18 `dfm_ring_breakout` warnings are new, from the newer hwde, on 0.5/0.3 mm vias with a 0.1 mm ring. None is a via this round added. Left as they are; it is a warning for JLC's drill wander, not a rule break.
