# DFM / mechanical / JLC assembly review (domain `dfm`)

Reviewer: independent DFM/assembly pass, 2026-10-28. Both boards. Read-only on `boards/`.
Scripts used are kept in `docs/review-2/sims/dfm/` (footprint dump, Gerber read, EP/via check).
JLC capability numbers below are from https://jlcpcb.com/capabilities/pcb-capabilities fetched 2026-10-28.
This file is written block by block; a block marked *(in progress)* is not finished.

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
`2-2X3` holes for the two body posts, 14/16 mm across the base. The front view shows posts reaching
4.5 mm below the base and the pins 3.5 mm. With no holes the posts stand on the board, the
base sits about 4.5 mm high, and the 3.5 mm pins can't reach through a 1.6 mm board. The jack can't be fitted.
**Fix:** use Lite's footprint on core and enlarge both post holes to the drawing's 2.0 x 3.0 mm
(NPTH slot >= 1.0 mm wide, which JLC allows). Keep the jack's front face at the board edge as on Lite.
Re-run the courtyard check after the change.

### DFM-2 (major, both): RCA post holes are smaller than the drawing, and the pin rows don't match it
Lite's post holes are 1.7 x 2.4 mm. The drawing says `2-2X3` (2.0 x 3.0), so each axis is 0.3-0.6 mm short.
The pin pattern in the footprint, which is the EasyEDA/LCSC land pattern, has the two signal pins
on rows 0.8 mm apart (y -1.95 and -1.15). The drawing shows both signal holes on one row (8 mm apart,
at 10.4 mm from the body face) and the centre hole at 7.3-7.95 mm. As I read the drawing, one
signal pin lands about 0.8 mm from its hole. The pins are 1.0 x 0.3 mm brass (diagonal 1.04 mm) in
1.6 mm holes, so each pin has about 0.28 mm of radial slack: an 0.8 mm error only fits if the pin bends.
The drawing is ambiguous here (it gives the 10.4/11.2 and 7.3/7.95 pairs without saying what they measure), so this is marked unverified, not wrong.
**Fix:** before ordering, print the footprint 1:1 on paper and put a real XDJK-0271-1650 on it,
or buy 2 pcs from LCSC (C7527522, $0.26). Make the post holes 2.0 x 3.0. If the pins don't
line up, change pin 1/3 to 1.6 x 2.4 mm plated ovals along y, so either reading fits. JLC's minimum
plated slot is 0.5 mm, and these PTH pads stay on a 2.5 mm annulus.

