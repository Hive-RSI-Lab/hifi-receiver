# DFM / mechanical / JLC assembly review (domain `dfm`)

Reviewer: independent DFM/assembly pass, 2026-10-28. Both boards. Read-only on `boards/`.
Scripts used are kept in `docs/review-2/sims/dfm/` (footprint dump, Gerber read, EP/via check).
JLC capability numbers below are from https://jlcpcb.com/capabilities/pcb-capabilities fetched 2026-10-28.
This file is written block by block; a block marked *(in progress)* is not finished.

## Summary

| ID | Sev | Board | What |
|---|---|---|---|
| DFM-1 | **blocker** | core | J4/J5 stacked-RCA footprint has no holes for the jack's two body posts (Lite's copy has them). The jack can't seat and its pins can't reach through. |
| DFM-2 | major | both | RCA post holes are 1.7 x 2.4 against the drawing's 2.0 x 3.0, and the EasyEDA pin rows (0.8 mm offset between pins 1 and 3) don't match my reading of the drawing. Check against a sample before ordering. |
| DFM-3 | major | core | TPA3221 (DDV, thermal slug on top) has no heatsink mounting holes. RthJA 44.8 C/W unheatsinked means about 2.5 W of dissipation at most. |
| DFM-4 | major | core Plus | U18 IRM-H638T looks up, at a lid at least 21.6 mm above it. Use the front-facing IRM-3638T as on Lite, or put a window in the lid. |
| DFM-6 | major | core | New USB4085 (THT): GCT/EasyEDA annulus 0.125 mm is below JLC's 0.15 multilayer minimum, so use 0.70 pads. 0.85 mm-pitch hand soldering risks VBUS-CC bridges, so ask for inspection. Edge 1.74 mm from the rear slots. |
| DFM-12 | major | core Ultra | TOSLINK ordered as PLR237/T8BK on a T10BK footprint. The T8BK's two 1.70 mm posts have no holes. The T10BK is out of stock. Draw the T8BK footprint. |
| DFM-7 | minor | Lite | New HRO TYPE-C-31-M-12 is rated 20 V (0 % margin at 20 V PD). Pegs must sit 5.79 mm from the top edge. |
| DFM-8 | minor | both | HASL with 0.4-0.5 mm QFN/DFN and 0.5 mm FFC. Order ENIG. |
| DFM-9 | minor | both | 100 % EP paste with open vias in the pads. U8's paste windows sit on its 16 vias. Window the paste to 50-70 % off the vias. |
| DFM-11 | minor | both | Merged PTH/NPTH drill file; core's 0.20/0.45 vias sit exactly at JLC's price threshold; core C5/C311 silk on the pads. |
| DFM-13 | minor | core | KF301 holes 1.4 against KEFA's 1.2 (+0.1). |
| DFM-5, 10, 14 | note | both | 3D models (Lite's paths broken; core's 470 uF model 4 mm short); no fiducials/tooling; EC11 shaft offset 0.25 mm, FFC and IR orientation unverified. |

Checked and passing, with margins: USB4105 footprint against GCT B4 on both boards (all features within 0.005 mm; the shortened pads give 0.245/0.275 mm to the NPTH against JLC's 0.20).
HR911105A, DB128V, TS-1187A and IRM-3638T footprints match their drawings. Lite CPL is 134/134 by hwde cpl_verify, including the polarised parts by role.
Gerber annuli, mask webs (0.20 / 0.176 against 0.10) and slots are inside JLC limits. Plus/Ultra DNP sets are consistent. Lite enclosure snapshot equals the board. Courtyards: 0 courtyard_pads errors on either board.

## JLC numbers used (fetched 2026-10-28)

| Rule | JLC value |
|---|---|
| PTH annular ring, 2-layer 1 oz | recommended 0.25 mm, absolute min 0.18 mm |
| PTH annular ring, multilayer 1 oz | recommended 0.20 mm, absolute min 0.15 mm |
| Via | hole 0.15 / dia 0.25 min; dia >= hole + 0.1; 0.2-0.25 mm hole with dia < 0.45 mm costs extra |
| Min plated slot width | 2-layer 0.5 mm, multilayer 0.35 mm; length >= 2x width |
| Min NPTH hole / NPTH slot | 0.50 mm / 1.0 mm ("draw NPTH in mechanical/keep-out layer") |
| Hole tolerance | PTH +0.13/-0.08 mm; NPTH slot +-0.2 mm; position +-0.075 mm |
| Soldermask bridge (green, 1 oz) | 0.10 mm |
| Pad to silkscreen | 0.15 mm |
| NPTH to track / PTH to track | 0.20 mm / 0.28 mm |
| Copper to routed edge / V-cut edge | 0.2 mm / 0.4 mm |
| SMT panel at JLC | 5 mm tooling edges, 2 mm tooling holes, 1 mm fiducials at 3.85 mm from the panel edge |

## Findings (worst first)

### DFM-1 (blocker, core): J4/J5 stacked RCA footprint has no holes for the jack's locating posts
`boards/core/lib/aiee.pretty/CONN-TH_XDJK-0271-1650.kicad_mod` as placed (J4 at 107.587,135.197; J5 at
125.587,135.197, both rot 180) has three PTH pads only (1.6 drill / 2.5 pad at local (-4,-1.95), (0,1.95),
(4,-1.15)). Lite's footprint of the same name has those three plus two NPTH ovals 1.7 x 2.4 at local
(+-7.65, +1.60). Courtyards differ too: core x +-8, y -3.45..21.8; Lite x -8.75..12.3, y -4.3..15.2.
The Lian Xin drawing (A-AV2-7A1, XDJK-0271-1650, LCSC C7527522) calls for `3-dia1.6` pin holes and
`2-2X3` holes for the two body posts, 14/16 mm across the base. The front view (zoomed at 300 dpi) shows the three pins
(1.0 mm wide) reaching 4.5 mm below the base. The two side legs are 1 mm-thick metal snap-in legs with hooked ends at x +-7.5
(between the 14 and 16 dimensions) and reach 3.5 mm below. With no holes the legs stand on the board, the base sits 3.5 mm high,
and the pins enter only 4.5 - 3.5 = 1.0 mm into the 1.6 mm board. They don't come through to the solder side, so the jack can't be
fitted or soldered.
**Fix:** use Lite's footprint on core and enlarge both post holes to the drawing's 2.0 x 3.0 mm
(NPTH slot >= 1.0 mm wide, which JLC allows). Keep the jack's front face at the board edge as on Lite.
Re-run the courtyard check after the change.

### DFM-2 (major, both): RCA post holes are smaller than the drawing, and the pin rows don't match it
Lite's post holes are 1.7 x 2.4 mm. The drawing says `2-2X3` (2.0 x 3.0), so each axis is 0.3-0.6 mm short.
The pin pattern in the footprint, which is the EasyEDA/LCSC land pattern, has the two signal pins
on rows 0.8 mm apart (y -1.95 and -1.15). The drawing shows both signal holes on one row (8 mm apart,
at 10.4 mm from the body face) and the centre hole at 7.3-7.95 mm. As I read the drawing, one
signal pin lands about 0.8 mm from its hole. (The pin length checks out: pins 4.5 mm below the base minus a 1.6 mm board gives the 2.9 mm
protrusion the enclosure worker used.) The side legs are 1 mm-thick metal snap-ins with a hook. A 1.7 mm slot leaves 0.7 mm for the hook, against the 2.0 mm the drawing asks for. The pins are 1.0 x 0.3 mm brass (diagonal 1.04 mm) in
1.6 mm holes, so each pin has about 0.28 mm of radial slack: an 0.8 mm error only fits if the pin bends.
The drawing is ambiguous here (it gives the 10.4/11.2 and 7.3/7.95 pairs without saying what they measure), so this is marked unverified, not wrong.
**Fix:** before ordering, print the footprint 1:1 on paper and put a real XDJK-0271-1650 on it,
or buy 2 pcs from LCSC (C7527522, $0.26). Make the post holes 2.0 x 3.0. If the pins don't
line up, change pin 1/3 to 1.6 x 2.4 mm plated ovals along y, so either reading fits. JLC's minimum
plated slot is 0.5 mm, and these PTH pads stay on a 2.5 mm annulus.

### DFM-3 (major, core Plus/Ultra): no way to fix a heatsink on the TPA3221 (U15, Ultra U16)
TI SLASEE9C (rev. May 2025) section 5 says the DDV package's heat slug is "located on the top side of the
device for convenient thermal coupling to the heat sink". The pin table lists the PowerPAD as "connect to
grounded heatsink, placed on top side", and Thermal Information gives RthJA 44.8 C/W on a JEDEC 4-layer board
with no heatsink against 5.5 C/W to a fixed heatsink. (The Features list still says "thermal pad located on
package bottom", which contradicts the rest of the document. The device table and pinout say top, and the core footprint has no EP, which is consistent with top.)
Core has only the four M3 corner holes H1-H4 (38.1/151.0, 124.2/217.1); U15 (46.087,161.697) and U16
(78.987,161.697) have no screw or clip holes near them. At 44.8 C/W the TPA3221 reaches Tj = 150 C from 40 C ambient at
(150-40)/44.8 = 2.5 W dissipated. At 90 % efficiency that is about 22 W of total output, so even moderate playback
needs a heatsink. TI's layout notes T1-T3 say the heatsink sets the distance to the bulk caps and must be
grounded to the PCB.
**Fix:** add two M3 (3.2 mm PTH, GND) holes per amplifier on the U15/U16 long axis, spaced to suit a
stock clip-on or screw heatsink. TI's EVM uses screws either side of the package; around 25-30 mm apart clears the
14 mm package and its 9.7 mm courtyard. Keep the heatsink's footprint (about 30 x 20 mm) clear of parts taller than 1.5 mm.
Name the heatsink part in the BOM as hand-fit. Contingency: a thermal-adhesive heatsink (no holes) is
possible, but the pad must still be grounded per TI, so the screw holes are the safer choice.

### DFM-4 (major, core Plus): IR receiver U18 IRM-H638T looks up, at the lid
Everlight DMO-0000008 rev 3 (IRM-H6XXT/TR2), p6: the top view shows the R1.5 lens dome on the top face
(5.0 x 4.0 x 3.8 mm body). It is a top-looking SMD receiver. U18 sits at (76.587, 217.697), 3.0 mm in from the
bottom board edge (y 220.697), looking up at whatever lid the Plus gets. No core enclosure exists in
`enclosure/` (only Lite), so the 25 mm lid figure the enclosure worker gave can't be checked here.
Core's tallest parts are C5/C6 (C311 on Ultra): 470 uF/50 V radials, 20 mm body per LCSC C88733 (50PX470MEFC10X20).
So the lid sits at least about 21.6 mm above the board top, and the IR has to pass through a hole or light pipe that tall. A front-facing receiver avoids this.
Pin map checked: 1 GND, 2 GND, 3 OUT, 4 Vcc (datasheet p1) matches the board nets (pads 1/2 GND, 3 /IR_IN, 4 /ui/IR_VCC).
Land pattern: Everlight recommends 1.5 x 1.1 mm pads, 4.8 mm inner gap (pad centres +-3.15), 2.54 pitch.
The footprint has 1.5 x 0.9 pads at x +-2.95 (pad 4 at +2.92), an inner gap of 4.4 mm. The lead tip (7.0 +-0.3 overall,
so +-3.5) still sits 0.2 mm inside the pad's outer edge (3.7), against Everlight's 0.4 mm. This is minor.
**Fix:** use Lite's front-facing THT IRM-3638T (C42421366, 3 pins at 2.54 mm, footprint already proven on Lite),
mounted at a board edge behind a front-panel window. If the SMD part stays, the lid needs a window above
U18 within the +-45 deg reception half-angle. Ultra moves the UI to the FFC panel, so this applies to Plus only.

### DFM-5 (note, both): 3D models (enclosure-fit input)
The enclosure worker said core's footprints don't name their models. That is not what the board shows. On core,
278 of 282 footprints name a model under `${KIPRJMOD}/../lib/aiee.3dshapes/`, and 241 of those files exist. Missing are
C0603_L1.6-W0.8-H0.8.wrl (36 parts) and SC-70-6_L2.0-W1.3-H1.0-P0.65.wrl (U3); H1-H4 have none.
Lite is the board with the gap: 26 footprints point at `lite-main/lib/aiee.3dshapes/`, which doesn't exist;
104 use KiCad library STEPs and 8 have no model.
The models that do exist aren't always the right height. Core C5/C6/C311 use CAP-TH_D10.0-H16.0 (16 mm) but the part
is 20 mm tall (C88733 = 50PX470MEFC10X20). Core and Lite J1 use a "USB-3.1-SMD_16P-L8.9" model, not the USB4105.
Any lid-height check driven by the models understates the caps by 4 mm.
**Fix:** copy core's `aiee.3dshapes` for Lite, and correct the 470 uF model to H20.

### Courtyards (check_courtyard / courtyard_pads), both boards
hwde `check_courtyard.py --pcb` on both boards as they stand (run 2026-10-29): **pass, 0 violations**
(reports `sims/dfm/courtyard_lite-main.json`, `courtyard_core.json`). My own check (`sims/dfm/crtyd.py`,
every pad's extent against its footprint's courtyard box, footprint rotation removed) agrees, at 0 on both
boards. Every 0603/0402 footprint in `core/lib/aiee.pretty` (C0603, R0603, L0603, LED0603-RD, C0402, R0402) and Lite's
LED0603-R-RD_WHITE has a courtyard that covers its pads. Lite's passives are KiCad stock `C_0603_1608Metric` /
`R_0603_1608Metric`, so the EasyEDA body-outline-as-courtyard bug isn't present on either board.
If a newer hwde reports courtyard_pads, it is reading something other than these two board files (for example
an EasyEDA cache footprint before lib_pull's fpfix). This needs re-checking against the version that reported it.

## USB-C receptacles

### As built: USB4105-GF-A-120 (C5184243), both boards (being replaced, see below)
Checked against GCT drawing USB4105 rev B4 (18/12/23), "Recommended PCB Layout", and KiCad's
`USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal` (same numbers). The board footprint is the KiCad pattern
shifted +1.30 mm in y. Every feature agrees to within 0.005 mm: pegs NPTH 0.65 at x +-2.89 for the 0.50 pegs (0.15 clearance);
front tabs 0.6 x 1.7 slot in a 1.0 x 2.1 pad; rear tabs 0.6 x 1.4 in 1.0 x 1.8 (4.18 apart); 0.3 x 1.15 signal pads;
pegs to rear tabs 3.68. The 0.6 mm-wide pads (A1/B12, A4/B9, B1/A12, B4/A9) were cut from 1.15 mm to
1.08 (Lite) or 1.05 (core) at the peg end. That leaves 0.245 mm (Lite) / 0.275 mm (core) of copper to the 0.65 NPTH,
against 0.175 mm uncut and JLC's NPTH-to-copper minimum of 0.20 mm. The cut is the right call: it costs 6-9 % of the solder length on 0.6 mm tails.
Slot widths 0.6 against JLC's 2-layer plated-slot minimum of 0.5 (20 % margin); length/width 2.8 and 2.3 (min 2); annulus 0.2 against 0.18 (11 %).
Edge placement against GCT's "PCB edge" line (2.60 from the rear tab centre, flush with the shell front):
Lite's edge is 0.095 mm beyond it (shell front recessed 0.095 mm inside the board). Core's edge (x 34.587) is 0.975 mm
inside it, so the shell overhangs the board edge by 0.975 mm. Both work. The overhang means that edge needs no panel rail or V-cut, see DFM-10.

### New: Lite J1 -> HRO TYPE-C-31-M-12 (C165948)
Drawing: HRO "TYPE-C-31-M-12" rev A (2020.12.08), LCSC datasheet for C165948. Its recommended layout, measured from the
drawing (origin at the peg centres, +y toward the board edge):
- 12 pads, 1.15 mm long, from y -1.64 to -0.50. Width 0.30 (8 pads) / 0.60 (4 pads: A1/B12 and B1/A12 at x +-3.20,
  A4/B9 and B4/A9 at x +-2.40). Narrow pads at +-0.25, +-0.75, +-1.25, +-1.75 (A5 -1.25, B8 -1.75, etc.).
- Peg holes 0.60 at x +-2.89 (5.78). The pegs are 0.50 (bottom view), so 0.10 mm clearance.
- Front shell slots 0.6 x 1.7 hole in a 0.9 x 2.0 pad at x +-4.325, y -0.50. Rear 0.6 x 1.4 hole in 0.9 x 1.7, y +3.68
  (4.18 apart). The annulus is 0.15 mm, below JLC's 2-layer absolute minimum of 0.18 mm, so use 1.0 mm wide pads (0.2 mm annulus) as KiCad's footprint does.
- PCB edge at y +5.79 from the pegs.
The LCSC/EasyEDA model (fetched 2026-10-29) has pads 1.30 long centred 0.77 mm behind the front-slot line
(the drawing puts them 0.57 mm behind), and 1.2 mm wide shell pads at the same +-4.325, 4.18 apart. JLC places this part from its own
model, and the HRO part is JLC's most-built USB-C, so a footprint pulled from EasyEDA (lib_pull) gets the right CPL by construction.
KiCad's `USB_C_Receptacle_HRO_TYPE-C-31-M-12` is also usable: pads 1.45 long, rear slot 0.6 x 1.2, pegs 0.65.
Its 1.2 mm rear slot is shorter than HRO's 1.4. The tab is 0.3 mm thick, so a 1.2 slot still clears it, but use 1.4.
Things the worker has to get right on Lite:
1. The edge: put Lite's top edge (y 60.323) 5.79 mm in front of the peg centres. The present USB4105 pegs are at 66.693,
   which gives 60.903, so the new part's pegs need to sit at y 66.113 (or the board edge moves).
2. Rating: HRO gives 20 V / 5 A. Lite takes up to a 20 V PD contract (D1 is SMBJ20A), so the margin on the connector's voltage rating is 0 %
   (USB PD allows 20 V +5 % = 21 V). The USB4105 it replaces is rated 48 V. The dielectric test is 100 V AC, so this won't fail in practice,
   but it is a datasheet-rating violation at the top PD level.
3. Temperature -30..+80 C against the USB4105's -40..+85.
4. In the CPL the part's model centre isn't the pad centre. Take the rotation and centre from cpl_verify after the swap, and re-run cpl_render.

### New: core J1 -> GCT USB4085-GF-A (C7095263), through-hole
Drawing: GCT USB4085 rev B (28/02/23), sheet 2 "Recommended PCB Layout":
- 16 contact holes, 0.40 drill in a 0.65 pad, in two rows 1.35 apart at 0.85 pitch. A row (A1 A4 A5 A6 A7 A8 A9 A12, left to right
  from the component side) sits behind the B row (B12 B9 B8 B7 B6 B5 B4 B1). A1 is above B12.
- Shell: front pair 0.6 x 2.1 slot in a 0.9 x 2.4 pad, centre 0.98 below the A row. Rear pair 0.6 x 1.4 in 0.9 x 1.7,
  3.38 behind the front pair. x spacing 8.65. PCB edge 1.74 in front of the rear slot centres.
- Contacts 0.22 +-0.05 wide x 0.15 thick, diagonal 0.27 nominal / 0.32 max, in a 0.40 hole.
  JLC's hole tolerance is +0.13/-0.08, so the worst-case finished hole is 0.32: 0 mm clearance against the max contact, 0.13 mm nominal.
- Pin length below the seating plane 1.60 (section A-A), flush with a 1.6 mm board.
The LCSC/EasyEDA model (C7095263, fetched 2026-10-29) is GCT's pattern exactly: 0.65 pad, 0.40 drill (annulus 0.125 mm),
shell pads 0.9 x 2.4 / 0.9 x 1.7 at +-4.325, 0.98 and 3.38. KiCad's `USB_C_Receptacle_GCT_USB4085` uses a 0.70 pad (annulus 0.15)
and puts the edge line at 1.74 from the rear slots (y 6.10 in its frame), the same as GCT.
JLC's limits: multilayer PTH annulus absolute minimum 0.15 mm (recommended 0.20). GCT/EasyEDA's 0.125 is **below** it.
A 0.70 pad meets it with 0 % margin and leaves 0.85 - 0.70 = 0.15 mm copper between neighbouring pads, which is also
the solder-mask web (JLC 0.10 minimum, 50 % margin). There's no room for the recommended 0.20 annulus at 0.85 pitch.
**Use 0.70 round pads on 0.40 holes** (KiCad's numbers). Expect JLC's DFM to query the 0.15 annulus, and accept it.
An alternative is 0.70 x 0.95 ovals along y: rows 1.35 apart leaves 0.40 mm between rows, which improves the fillet but not the minimum annulus.
Can JLC assemble it? It is a Plugin (THT) part, Extended, stock 2623, $1.41. JLC solders THT by hand or selective wave and
charges per joint. Core already carries THT parts (RJ45 J2, RCA J4/J5, terminals J6/J7, C5/C6, EC11 SW1), so the THT setup fee
is already paid and J1 adds 20 joints. Risk: 16 joints at 0.85 mm pitch with 0.15 mm gaps, soldered by hand or wave, will bridge
unless done carefully, and the bridges sit under the shell where they're hard to see. A VBUS (A4/A9) to CC (A5) bridge puts up to 20 V on CC.
**Fix/contingency:** in the JLC order remark, ask for "J1 USB4085 0.85 mm pitch, selective solder or hand solder,
inspect A4-A5 / B4-B5 / B9-B8 for bridges". At bring-up, measure CC1/CC2 to VBUS and to GND before the first PD connection (bench script).
Also check that the THT part isn't placed where wave-solder pallets would shadow nearby SMD parts on the bottom (core is single-sided, so OK).
Edge: core's left edge is x 34.587. With the part at rot -90 (mouth facing -x), the rear slot centres must sit 1.74 mm
inside the edge, at x 36.327.

### CH340C -> CH340B (C81010), Lite U6 / core U8
Same SOP-16 body. The EasyEDA models (C81010 `SOIC-16_L9.9-W3.9`, C84681 `SOP-16_L10.0-W3.9`, fetched 2026-10-29) both
have pin 1 at (-4.445, +y), pin 16 at (-4.445, -y) and pin 9 at (+4.445, -y), with the same centre. Their pad rows are +-2.736 against +-2.872 and
the pads 1.97 against 1.75 long. The board footprint is the CH340C one (pads 0.56 x 1.745, rows +-2.87, pitch 1.27 at +-4.45),
which fits any 150-mil SOIC-16 (JEDEC MS-012 leads 0.31-0.51 wide, 6.0 nominal toe-to-toe), so it suits both. The CPL rotation carries over unchanged
(Lite U6 90, core U8 90). Pin 1 is GND on both boards, which the swap leaves alone. CH340B is Extended, stock 5088, $0.91.
The pin-function differences (CH340B's internal EEPROM, pins 7/8/15) belong to the digital review.

## Fab data: my own read of the Gerber zips (gerbonara 1.6.3 + shapely, `sims/dfm/gerb.py`)

| | Lite `lite-main_gerbers.zip` | core `core_gerbers.zip` |
|---|---|---|
| Layers | F/B Cu, Mask, Paste, Silk, Edge.Cuts (2 copper) | F/In1/In2/B Cu + the same (4 copper) |
| Job file | 100.1 x 90.1 mm, 1.6 mm, HASL | 120.1 x 100.1 mm, 1.6 mm, HASL, ImpedanceControlled true, stack 35 um / 0.2444 1080x3 / 15.2 um / 1.065 core / 15.2 um / 0.2444 / 35 um |
| Drill file | one `.drl`, `FileFunction MixedPlating` (PTH+NPTH in one file) | same |
| Smallest via | 0.30 / 0.60 (165 + vias); ring 0.15 | 0.20 / 0.45 x21 (ring 0.125); 0.30 / 0.60 x367, 0.30 / 0.70 x260 |
| Smallest component PTH | 0.30 drill / 0.61 pad (U8 thermal vias, ring 0.155); 1.0 / 1.8 (U9) ring 0.40 | 0.914 / 1.524 (U19) ring 0.305; 1.0 / 1.6 ring 0.30 |
| Plated slots | 0.6 x 1.7 / 0.6 x 1.4 (J1 tabs), ring 0.20 | same 4 + EC11 1.7 x 2.8 |
| NPTH | 0.65 x2 (J1 pegs), 1.7 x 2.4 slots x4 (RCA posts) | 0.65 x2, 3.3 x2 (RJ45 pegs) |
| Min solder-mask web | 0.20 mm (8 places) | 0.176 mm (8, U1 CH224Q 0.4 pitch) |
| Silk on mask openings | 0 | 1.59 mm2: C5 pads (60.15, 140.5)/(65.23, 140.5) and C311 pads (93.05, 151.5)/(98.13, 151.5), 0.40 mm2 each |

Margins against JLC:
- Vias: Lite 0.30/0.60 against "dia >= hole + 0.1", so +0.2 margin; standard price. Core 0.20/0.45: JLC charges extra for "0.2-0.25 mm hole with
  via diameter **less than** 0.45", so at exactly 0.45 it's standard price with 0 margin. If a CAM rounding makes any of the 21 vias 0.449,
  the order moves to the small-via price tier. **Minor fix:** make those 21 vias 0.20/0.50, or 0.30/0.60 where there's room.
- PTH annulus: Lite's component PTHs are 0.40-0.45, far above the 0.18 2-layer minimum. Lite's USB tab slots are 0.20 (11 %).
  The U8 footprint thermal vias (0.155) are GND vias inside the EP; JLC's CAM treats 0.3 mm holes as vias. Core component PTHs are 0.30 minimum against 0.15 (100 %).
- Mask webs: 0.20 / 0.176 against JLC's 0.10 green minimum, 100 % / 76 % margin. pad_to_mask_clearance is 0, so the openings are 1:1.
  JLC's LDI 1:1 process (June 2025) supports that.
- Silk: Lite is clean. On core the C5/C311 silk (the reference text or polarity mark, which C6's copy doesn't have) lands on the pads.
  JLC clips silk off pads, so the only risk is losing that marking. **Note:** move it 0.15 mm off the pads.
- **(DFM-11)** Merged drill file: JLC asks for NPTHs "in the mechanical layer or keep-out layer" and its KiCad guide generates separate PTH/NPTH files.
  The NPTH holes here have no copper (pad = drill), so JLC CAM will usually keep them unplated. If they get plated, the 0.65 USB peg
  holes shrink by about 2 x 18 um (JLC's average plating). The pegs are 0.50, so they still fit (0.11 mm margin left), and the RCA post slots
  are unaffected in practice. **Minor fix:** export with "PTH and NPTH in a single file" off.
- Copper to edge: JLC's minimum is 0.2 mm (routed). On Lite, J1's mouth-end shell tab pads (1.0 x 1.8 at y 63.013) reach y 62.113,
  1.79 mm from the top edge (y 60.323), so the margin is 1.59 mm. On core they reach x 36.207 - 0.9 = 35.307 against the edge at 34.587: 0.72 mm, margin 0.52 mm.

### DFM-8 (minor, both): HASL with 0.4-0.5 mm pitch leadless parts; order ENIG
Fine-pitch parts: Lite U7 ES8388 QFN-28 0.45 pitch (pads 0.23 wide), U8 TPA3118 HTSSOP 0.65. Core U1 CH224Q DFN-10 0.40
(pads 0.224), U13 ES7243E QFN-20 0.40, U9 LAN8720A QFN-24 0.50, U10 ES9018K2M QFN-28 0.50, U14 MSOP-10 0.50, J3 FFC 0.50 (Ultra), U15/U16
HTSSOP-44 0.635. Lead-free HASL leaves a domed, uneven deposit that grows on small pads, and on a 0.224 mm pad at 0.4 mm pitch that
means tombstoned/floating DFN/QFN and opens. JLC's capability page itself requires ENIG for 0.2-0.25 mm BGA pads and warns of
"high risk of short circuit with HASL" for exposed fine features. Both job files say HASL.
**Fix:** order ENIG on both boards. The surcharge for a 5-board order of these sizes is a few tens of dollars at most
(exact figure from JLC's quote page; not fetched, so unverified). It also gives the flat finish the 0.5 mm FFC contacts want.

### DFM-9 (minor, both): exposed-pad paste and open vias in pads
No footprint sets a paste margin/ratio (0 occurrences on either board), so every EP gets a 100 % paste aperture except Lite U8:
- Lite U8 TPA3118 EP 3.81 x 3.71 has four 1.4 x 1.4 windows (7.84 / 14.1 mm2 = 55 %). But all 16 thermal vias (0.3 drill, open,
  `*.Cu` only, no mask) sit inside those windows, 4 per window, so the paste goes straight onto the vias and wicks to the bottom.
- 100 % paste with open 0.3/0.6 vias in the pad: Lite U7 ES8388 (EP 2.4 x 2.4, 1 via), Lite U1 CH224A (3.3 x 2.1, 3 vias),
  core U10 ES9018K2M (3.25 x 3.25, 9 vias), core U9 LAN8720A (2.55 x 2.55, 1 via). Core U13 ES7243E (1.65 x 1.65) and U1 CH224Q (1.4 x 0.9) have 100 % paste and no vias.
- Core U7 ESP32-WROVER pad 2 (+3V3) has an open 0.3/0.6 via in the 2.0 x 0.9 pad.
With 100 % paste on a QFN EP the part can float on the EP solder and lift the 0.4-0.5 mm-pitch perimeter joints. The open vias
pull solder away from the EP and leave balls on the bottom side (no bottom parts, so cosmetic).
**Fix:** set the EP paste to 50-70 % as a grid of windows that avoids the vias (KiCad pad `solder_paste_margin_ratio` -0.2 to -0.3, or
explicit windows as on U8), and on U8 move the windows off the vias (3 x 3 grid between the via rows). Alternatively order JLC's
epoxy-filled, capped via-in-pad process (offered for 0.15-0.55 mm vias). That costs more on 2-layer, so windows are the cheap fix. Move the U7 ESP32 pad-2 via
off the pad by about 0.5 mm.

### DFM-10 (note, both): no fiducials, no panel, no tooling holes
Neither board has fiducial footprints (0 matches) or tooling holes; the four M3 holes are the only mechanical holes. JLC's
capability page: "For SMT assembly at JLCPCB, use 5 mm tooling edges, 2 mm tooling holes, and 1 mm fiducials centred at 3.85 mm from the
panel edges". When JLC panelises for assembly it adds the rails, fiducials and tooling holes itself, and single boards
without fiducials are assembled using pads as references. This works but limits placement accuracy on the 0.4 mm DFNs. **Fix (cheap):**
add three 1 mm copper / 2 mm mask fiducials per board in free corners, at least 3.85 mm from the edges. Core's J1 shell overhangs the left edge by 0.975 mm
(see the USB block), so ask JLC for rails on the top/bottom edges only, or keep the edge rails off the USB edge.

## Other connectors and THT parts

### DFM-12 (major, core Ultra): TOSLINK U19 is ordered as PLR237/T8BK on a PLR237/T10BK footprint, and the T8BK has two locating posts with no holes
BOM/CPL Ultra: U19 `PLR237/T8BK`, LCSC C17223620 (stock 236), footprint `OPTO-TH_PLR237-T10BK`.
Everlight datasheets:
- PLR237/T8BK, DPL-0000262/263 Rev.1 (2021-03-09), p5. Pins 1 Vout, 2 GND, 3 Vcc (0.80 holes, 2.54 pitch). Two NC pins 4/5
  (1.10 holes, 5.08 apart, 2.63 from the signal row). **Plus two 1.70 +-0.05 locating-post holes, 5.60 apart, 2.61 beyond the NC row.**
- PLR237/T10BK, DPL-0000261 Rev.1 (2021-03-30), p5. Pins 1 VCC, 2 GND, 3 VOUT. NC pair 1.10 holes, 5.08 apart, 2.62 from the signal row.
  No post holes. Side view: pin rows 6.14 / 8.76 from the front (T8BK: 5.88 / 2.61 / 0.85 / 2.525 stack, so a different front offset).
The pin numbers are mirrored between the two datasheets: T8BK's bottom view has 3-2-1 left to right, T10BK's has 1-2-3, each with the port
toward the bottom of the view. So physically the same pin carries Vcc in both, and the board (pad 1 = /linein/OPT_VCC at x 142.527, pad 3 =
/OPT_SPDIF) is electrically right for either part, assuming the port faces the top edge. The NC row (y 129.887) sits nearer the edge than the signal row (132.507), as in both drawings.
The failure is mechanical. The board has 0.914 holes for pins 1-3 and 1.1 holes for 4/5, but nothing for the T8BK's two posts. A post that
hits the board raises the body (the top view gives 1.70 for the post) and tilts the optical port. The 3.85 mm pins still reach through
(3.85 - 1.70 = 2.15 > 1.6), so JLC's hand solderer will probably solder it crooked rather than stop, and the port will sit about 1.7 mm higher than
the enclosure opening expects.
The T10BK the footprint was drawn for is C17427482, **stock 0**. That is presumably why the BOM fell back to the T8BK.
**Fix:** draw a T8BK footprint: pins 1-3 0.8-0.9 drill at 2.54; NC 1.1 drill at +-2.54, 2.63 from the signal row; add two 1.70 PTH
or NPTH at +-2.80, 2.61 further out (local y +3.92 if the signal row is at -1.31). Rename it `OPTO-TH_PLR237-T8BK` and re-check the port-to-edge
distance against T8BK's side view, which differs from T10BK's. Keep the pin-1 = Vout numbering of the T8 datasheet in the symbol map, and check
it in the digital/parts review: with T8 numbering, pad "1" must be on the OPT_SPDIF net, not on VCC. If the worker renumbers pads to T8
numbering without swapping nets, Vcc and Vout swap.

### Connector-by-connector check (board footprint against the maker's drawing)
Every connector footprint below is the LCSC/EasyEDA land pattern of its LCSC number. I fetched the EasyEDA models of C12074, C474881,
C361165, C318884 and C2856803 on 2026-10-29, and their pad numbers, positions and sizes equal the board's. So JLC places them as drawn, and
the remaining question is whether that pattern matches the maker's drawing.

| Ref | Part | Drawing | Board footprint | Result / margin |
|---|---|---|---|---|
| core J2 | HanRun HR911105A (C12074) | HanRun HR911105A rev A/2, p3 layout | Signal 1.0 drill (rec. 0.89), LED 1.1 (rec. 1.02), shield 1.8 (rec. 1.63), pegs NPTH 3.3 (rec. 3.25); pegs 11.44 apart, shield 15.48, LED 13.26 / 8.18, rows 2.54 at 6.35 / 8.89 from the pegs, LED row 4.89 the other side | All within 0.05 mm of the drawing. Pin 1 is on the row next to the pegs, rotated 180 deg from the drawing view, not mirrored (pin 1 right becomes left and LED 12 left becomes right, consistently). Front face 10.92 from the pegs = x 34.167, which puts the jack 0.42 mm past core's left edge (34.587); HanRun's panel cutout assumes the face at the panel. Drill clearances +0.11 / +0.08 / +0.17. OK. Rated 0-70 C. |
| core J6/J7 (Ultra J10/J11) | KEFA KF301-5.0-2P (C474881) | KEFA KF301-5.0 rev A 2021-03-13 | 2 pins, 5.0 pitch, 1.4 drill / 2.2 pad | KEFA asks for 1.2 (+0.1/0) holes for 1.0 pins. **(DFM-13)** 1.4 is 0.2 over KEFA's maximum; it solders (0.4 mm diametral gap) but takes more solder. **Minor:** 1.2-1.3 drill. Rating UL 300 V / 16 A, 22-14 AWG; IEC 17 A, 1.5 mm2. Speaker current at 100 W into 4 ohm is 5.0 A rms, so a 69 % margin to 16 A. Pin row 5.5 mm from core's top edge (y 120.697); KEFA puts the pin row 4.0 / 3.6 mm from the body faces, so the body face sits 1.5-1.9 mm inside the edge. |
| Lite J4 | Dorabo DB128V-5.08-4P (C2915641) | Dorabo DB128V-5.08 T0-1 2022-10-27 | 4 pins, 5.08 pitch, 1.5 drill / 2.3 pad | Dorabo asks for 1.60 holes for 0.80 x 1.00 pins (diagonal 1.28). 1.5 gives 0.22 nominal and 0.14 worst case (JLC -0.08), OK. Rating UL 300 V / 16 A, 22-12 AWG; IEC 24 A, 2.5 mm2. Lite's amp output is about 3.5 A rms max (50 W / 4 ohm), so a 78 % margin. The 18 AWG ferruled wires in the enclosure plan are within 22-12. Pin row 5.6 mm from the edge, body face 5.0 from the pins, so 0.6 mm inside the edge. Height 14 mm (enclosure uses 14). |
| Lite J2/J3, core J4/J5 | XDJK-0271-1650 | Lian Xin A-AV2-7A1 | see DFM-1 / DFM-2 | core: no post holes (blocker). Both: post holes and pin rows to verify. Rated 50 V / 0.2 A, -40..70 C, fine for line level. |
| core Ultra J3 | XUNPU FPC-05F-20PH20 (C2856803), 0.5 mm, bottom contact, flip lock, H2.0 | LCSC PDF is corrupt (fails pdftotext and pypdf), so I compared against the EasyEDA land pattern only | 20 x 0.30 x 1.25 at 0.5 pitch (y -1.29), tabs 2.0 x 2.5 at x +-6.44 (y +1.29) | Same as EasyEDA. Mask web 0.20 (JLC 0.10). HASL on a 0.30 mm pad is the case for ENIG (DFM-8). Bottom contact: the FFC pin-1/pin-20 orientation and contact side at the panel end aren't verified, and need a same-side or reversed (type A/B) cable chosen to suit. That is a known-unknown for the UI panel. |
| Lite U9 | Everlight IRM-3638T (C42421366) | Everlight DMO-0000371 rev 4 | 3 pins at 2.54, 1.0 drill / 1.8 pad, pin 1 square | Pinout 1 OUT, 2 GND, 3 Vcc matches the nets (1 /IR_IN, 2 GND, 3 /IR_VCC). Side-looking part; the lens direction relative to the pin row isn't shown on the board. The enclosure fit says "lens forward". The pin row is 3.0 mm from the front edge. Unverified: the body depth/lens offset from the pins (the drawing page I could get had no dimensions). |
| core U18 | IRM-H638T | see DFM-4 | | |
| core SW1 (Plus) | Alps EC11 vertical, with switch, LCSC C361165 = **EC11E1834403** (footprint is named EC11E1820402) | Alps EC11 catalogue p279 mounting detail (EC11 family) | A-C-B at 2.54 pitch, row +7.25; D/E 5.08, row -7.25; tabs 1.7 x 2.8 slots at x +-6.25 | Alps gives 2.5 / 5.0 pitch and rows 7.5 / 7.0 from the shaft (14.5 total, same as 7.25 + 7.25). So the footprint centres the rows and the shaft sits 0.25 mm off the footprint origin. Pitch error 0.04 mm per outer pin in 1.2 holes (about 0.6 mm pins), fine. **Note:** the knob hole in a panel should be cut at the shaft position (origin shifted 0.25 mm toward the A-C-B row), not at the footprint origin. Whether the EC11E1834403 has detents (Alps table: "无" in the detent column for 1834403) is a UX question for the product owner. |
| Lite SW1-3, core SW2/SW3 | XKB TS-1187A-B-A-B (C318884) | XKB TS-1187A-X-X-X A0 | 4 pads 1.0 x 0.75 at x +-3.0, y +-1.85 | XKB: 5.0 / 7.0 spans (x +-3.0) and 3.0 / 4.5 (y +-1.875), a 0.025 mm difference, OK. A-B (pads 1-2) and C-D (3-4) are internally connected; nets BTN / GND match. H = 1.5 (code A), travel 0.25 mm, 50 mA / 12 V. |

THT and assembly at JLC:
- Lite THT: J2, J3 (3 joints each), J4 (4), U9 (3), 13 joints. Core Plus: C5/C6 (4), J2 (14), J4/J5 (6), J6/J7 (4), SW1 (7), 35 joints; with the
  USB4085, 55. Ultra: C5/C6/C311 (6), J2 (14), J4/J5 (6), J6/J7/J10/J11 (8), U19 (5), with the USB4085 59. JLC solders these by hand or selective
  wave and charges per joint plus a THT setup (the exact rates weren't fetched, so unverified).
- `BOM-full.csv` marks every THT part `smt_placed`, and Ultra's parts-ultra.json marks U19 the same. JLC classifies by its own part data,
  so this is a label problem only. It does mean hwde's cpl_verify judges these THT parts by their EasyEDA pad numbers like SMD parts. U19 has
  **no EasyEDA model** (C17223620 returns nothing), so its CPL rotation can't be checked by script.
- All parts are top side, and the bottom paste layers are empty (454-byte gbp files). This is single-sided assembly, so no second reflow pass.
- Extended parts: Lite 19 lines, core Plus 33, Ultra 34. JLC's $3 per extended line comes to about $57 / $99 / $102 per order
  (same LCSC data; the fee isn't fetched, so unverified). Low stock seen 2026-10-29: ES9018K2M C2688377 706 pcs; PLR237/T8BK C17223620 236 pcs;
  PLR237/T10BK C17427482 0 pcs. Task 261029-022243 covers stock in depth.
- Board 1.6 mm / 1 oz outer copper against the connectors: USB4105-GF-A-**120** stakes 1.20 +-0.15 < 1.6 board, so they don't protrude (OK);
  USB4085 contacts 1.60 below the seat, flush; RCA pins 3.5; DB128V pins 3.5; KF301 pins 4.0; HR911105A 3.05. All reach through 1.6.

## Enclosure (Lite only; `enclosure/` docs treated as claims)
- **Snapshot vs board:** I diffed `enclosure/lite-main.board.json` (the README says it is the 15:03 board, "before its final route") against today's
  `lite-main.kicad_pcb`. All 4 holes and every connector, switch, LED and the IR receiver are at the same position and rotation.
  Only 6 small parts moved (Q4, R16, C10, C11, R7, D1). The tallest of those is D1 (SMB, about 2.3 mm), far under the lid. So `fit.md`'s passes
  apply to the present board. Re-run it after the J1 swap and the RCA footprint change (DFM-2).
- **Mounting:** H1-H4 are M3 (3.2 drill, 6.4 GND pad, 8 stitching vias) at 3.55 mm from both edges, at (3.55, 3.55), (96.45, 3.55),
  (3.55, 86.45), (96.45, 86.45) in board coordinates. They match the enclosure's pillars (fit.md "lid pillar lands on H1..H4").
  The M3 screw head clamps the board between the base standoff and the lid pillar (2.5 mm pilot in plastic, per the README).
- **Heights against the lid** (board top to lid 36.0 mm): RCA pair 27 mm (9.0 mm clear); DB128V 14; C58 470 uF polymer 12.5 max (MA35V470M10X13,
  10 x 13 can per LCSC); C59 10.2; L1-L4 5.0; U9 about 8. The worst margin is the RCA at 9.0 mm, for the binding-post nuts above it.
- **USB-C:** opening 13.4 x 7.4, wall 2.4 + 0.6 gap, so the outer wall face is 3.0 mm in front of the board edge. The USB4105 mouth sits 0.095 mm inside the
  board edge, so the plug must reach 3.1 mm through the wall. The opening is sized for a 12.4 x 6.5 overmold to enter, so most cables mate.
  When J1 becomes the HRO part, keep its shell front at the board edge (DFM-7) or the opening moves.
- **RCA barrel axes 6.5 / 20.5 mm above the board** are the enclosure worker's hand correction of a model they say is 13.5 mm low. The Lian Xin drawing
  gives 14 +-0.1 between the axes (20.5 - 6.5 = 14.0, consistent) and a 27 mm body. I couldn't read the B-axis height above the base off the
  drawing with certainty, so the 6.5 is unverified. A 12 mm hole round an 8.3 mm barrel tolerates about +-1.8 mm.
- **Core / Plus / Ultra:** no enclosure exists yet. Inputs for it: tallest parts C5/C6/C311 at 20 mm (not the 16 mm in the 3D model, DFM-5),
  RCA pairs 27 mm, HR911105A 13.5 mm (HanRun C dimension), TPA3221 heatsink (DFM-3), U18 IR looking up (DFM-4).
  Core's left edge has J1 (USB4105 shell 0.975 mm past the edge) and J2 RJ45 (face 0.42 mm past the edge), so that edge can't carry a panel rail.

## CPL: rotation and pin 1 against JLC's (LCSC/EasyEDA) models
Tool: hwde `cpl_verify.py` (fits each part's own EasyEDA model to the board footprint by pad number, plus by role for diodes, LEDs and polarised caps).
Cache in `sims/dfm/easyeda-cache`, reports `sims/dfm/cpl_{lite,plus,ultra}.json`, re-run script `sims/dfm/cpl_core.sh`.
- **Lite: 134 / 134 parts pass** (status pass, 0 failed). This includes the polarised parts by role: D1 SMBJ20A at 180, D2 LED at 0, C58 polymer
  at 270, C59 at 270, and every IC's pin 1 (U1 CH224A, U2-U4, U5 ESP32, U6 CH340C at 90, U7 ES8388, U8 TPA3118, U9).
  J2/J3 (RCA) show a 0.79 mm model-centre offset. That is the EasyEDA model's origin, not an error, and as THT parts they're placed by their holes.
- **core:** *(in progress; EasyEDA rate-limits fetches and the re-run loop is still going)*. So far Plus is 192 of 230 OK and Ultra 225 of 272, with 0 wrong.
  The verified ones include every polarised part: D1 SMBJ30A, D2 BZT52C33, D4 BZT52C15, D3 LED, and C5/C6/C311 radial 470 uF.
  Not yet fetched: U1-U4, U9-U15/U16, U17, U18/U19, Y1/Y2 and some resistors. U19 PLR237/T8BK has **no EasyEDA model**, so it can't be checked by script,
  and as a THT part it's placed by hand.

## Plus vs Ultra BOM/CPL (DNP consistency)
Built from `core/fab/BOM.csv` + `CPL.csv` (Plus) and `core/fab/ultra/` (Ultra), diffed line by line:
- Ultra-only, present in Ultra's BOM and CPL, absent (DNP) from Plus's: U16 TPA3221 with L5-L8, C301/C302/C304/C306-C312/C316-C324/C326/C327/C329/C330/C332/C333/C335,
  R303/R305/R313-R315/R325/R328/R331/R334, FB3, J10/J11 terminals, J3 FFC, R402/R403, U19 TOSLINK and C126 (OPT_VCC decoupling). Consistent in both files.
- Plus-only (DNP on Ultra, which moves the UI to the FFC panel): SW1 EC11, SW2/SW3, U18 IR, C400, R400. Consistent in both files.
- Every other line is identical in value, LCSC and position (CPL diff shows only those references). No reference appears in one variant's
  BOM and not in its CPL.
- Cosmetic: in both BOMs the J10/J11 line's Comment says "SPK A L" (it's amp B), and BOM-full gives DNP rows an "smt_placed"-style MPN = value.
  No effect on assembly.

