# core amp-area ECO: R3-AMP-1, R3-AMP-2, R3-LAYOUT-1

Task 261030-215553 (issue #121). This is a layout ECO on the routed board. It
runs from the board as it was before the ECO, in order:

1. `boards/core/kicad/gen/amp_eco.py`
2. `amp_in_eco.py`
3. `j3_model.py` Checked by round 2's `docs/review-3/sims/amp/check_amp_layout.py`
(16 fails before, 0 after), drc_routed (0) and dfm (pass, with 10 waivers, see below).

## R3-AMP-2: PVDD HF caps at the pins

The top side around each pin group is taken by the output pours, the PVDD
strips and the BST pins. So each pin group's pair goes on **B.Cu, directly
under the group** (TI's own fallback in the finding):

- **1 uF:** now 1 uF/100 V X7R 1206 (C282823, TCC1206X7R105K101HT, 160k in stock, extended part). This answers the DC-bias point: the 0805/50 V part keeps ~40-60 % of its value at 28 V. The PVDD pad sits under pins 29-31 (36-38) and is tied to the strip's nearest plane via. The GND pad lands on the EP via field.
- **100 nF/50 V 0603:** sits beside the 1206, outboard, joined by a 1.5 mm B.Cu track. A 1.5 mm B.Cu run from the 1206 pad goes up through the strip's plane vias.
- **Plane vias:** each strip's vias are re-laid as two staggered columns above the pad, 8 where there were 5-6 (check_current: ≥ 1 via per 0.5 A).

Pad to pin, centre to centre: 1206 1.91 mm, 0603 2.40 mm (was 6.5-8.1 mm).
The loop runs: pin → F.Cu strip → plane via → B.Cu pad → cap → EP vias → EP.
The pair is moved, not doubled: the old row-end spots on top are where the BST caps now sit.

## R3-AMP-1: bootstrap caps at the row ends

Pad to pin, with BST copper per net and width:

| Cap | Where | BST pad to pin | BST net | OUT side |
|---|---|---|---|---|
| C223/C321 (BST1P) | left end, vertical | 1.32 mm | 1.3 mm, 0.3 mm | pad inside the OUT1P pour |
| C224/C322 (BST2M) | right end, vertical | 1.32 mm | 1.3 mm | pad inside the OUT2M pour |
| C222/C320 (BST1M) | left end, below | 2.52 mm | 3.0 mm, no via | 0.45 mm via under pin 35, B.Cu 0.4 mm: 10.5 / 11.4 mm |
| C225/C323 (BST2P) | right end, below | 2.52 mm | 3.0 mm, no via | via under pin 32, B.Cu 0.4 mm: 10.3 / 11.0 mm |

Before: BST traces up to 35.6 mm with vias, and OUT links of 19-34 mm.
OUT1M (pin 35) and OUT2P (pin 32) leave mid-row, between PVDD groups, so a
≤ 5 mm stub to them can't be laid from a row end. Their link instead takes a
0.45/0.2 mm via just under the pin, between the pin row and the EP. It then
runs on B.Cu under the PVDD 1206's body, between its pads.

Side changes:

- U15's A_AVDD and AMP_OSCM B.Cu doglegs at the pin-23 end were re-routed. AVDD's old F.Cu leg also ran where C224 now sits.
- A dead B_OUT1P strap of U16's was removed.
- C7 (zone B PVDD 10 uF) used to reach PVDD through the old C307 spot. It now has its own In2 plane via at (93.1, 161.7), deep enough in the plane for check_current's 1.75 mm erosion.

## R3-LAYOUT-1: input caps at the IN pins (U15 done, U16 not)

**U15:** C218 and C220 are on B.Cu under pins 8 and 15. Each IN_P node is now pin → F.Cu → 0.45 mm via → B.Cu → cap:

| Net | Now | Before |
|---|---|---|
| A_IN1P | 6.9 mm | 45.2 mm |
| A_IN2P | 9.3 mm | 55.8 mm |

The old IN_P copper had the run's length (old cap spot near U16, to U15). It is now AMP_IN_L/R, the op-amp side, with its last mm at the pin cut and the old cap spot bridged. AMP_IN_L/R are now 91/116 mm. They are low-impedance, which is what the finding asked for.

Not done:

- **The ≥ 3 mm from OUT/BST and "F.Cu over In1" parts.** IN2P's B.Cu leg references In2.
- **U16** (C316/C318, C317/C319). Its IN_P runs are 13.5 and 24.6 mm, routed under its own body. B.Cu under U16's signal row has no room for the caps without a wider re-route. **Open for the next touch.**

## Two-sided assembly

The ten bottom parts make core a two-sided JLC build (`fab/ORDERING.md`).

- **Cost:** a second assembly side, plus one extended line (C282823).
- **dfm:** hwde's cpl_verify fails every bottom part as `bottom_unverified`, because JLC's bottom convention isn't modelled. All ten are non-polar 2-pad MLCCs, and the CPL axis was checked to match the board for each. `boards/core/reports/dfm-waivers.json` waives them by ref, until 2027-04-30.
- **Wiring:** `tools/hwcheck.py` and `kicad/gen/fab.sh` pass that file to the dfm gate.
- **At order:** confirm in JLC's bottom-side preview.

## Also in this change

**J3** (`j3_model.py`): the FFC connector's 3D model is offset 0.45 mm, as hwde's check_model_fit asks. This fails on main too with /hive/tools/hwde.

Result: core passes erc, drc_routed, verify and dfm (dfm with the 10 waivers above).
