# core: full GND stitching (2026-11-05)

The person looked at the Lite's render on the JLC cart and asked why so little ground stitching
showed, and for a full pass on the Lite and core. Core had 385 GND vias. `stitch_vias` at the auto pitch
(4.28 mm, from a 1 ns edge) planned 278 more, 0.6/0.3 mm at 0.2 mm clearance. 272 went in, for 657.
The plan and the dropped vias are in `boards/core/reports/stitch_full.json`.

**Why 6 were dropped.** A GND via through a power pour on In2.Cu cuts a hole about 0.96 mm across.
With all 278, `check_current` failed `pour_neckdown` on two 5 A rails: VBUS at 2.30 mm and PVDD at
3.33 mm, against 3.50 mm needed. The board without the new vias was clean. `kicad/gen/stitch_necks.py`
tests each planned via in those pours one at a time and keeps it only if the pour still carries its
width. It kept 3 of 3 on VBUS_IN, 0 of 1 on VBUS and 63 of 68 on PVDD. These are the vias dropped:

| x (mm) | y (mm) |
|---|---|
| 62.925 | 174.58 |
| 67.208 | 157.449 |
| 88.622 | 157.449 |
| 92.905 | 157.449 |
| 71.491 | 166.015 |
| 62.925 | 170.297 |

`pour_neck`'s reported position is a representative point, not the pinch, so choosing vias by eye
from it doesn't work. Test each via instead.

**Results.** DRC, verify and DFM pass. No new via lands in an SMD pad. BOM and CPL are unchanged.
Nothing has been built or measured.

**Ordering.** These are 0.3 mm holes, so the "fill and cap all 0.2 mm and 0.3 mm holes" remark in
`boards/core/fab/ORDERING.md` covers them: 1182 filled vias in all, listed with the in-pad ones in
`boards/core/fab/VIA-FILL.md`. Any quote made with the older Gerbers needs replacing, and the fill
charge should be read again on the quote page.
