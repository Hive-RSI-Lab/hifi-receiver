# lite-main: ESD clamps at the connectors people touch (issue #133, Lite half)

**Finding.** hwde's `check_esd` (TI SLVA680: put the clamp first, right at the
connector, with a short path to ground) found 12 unclamped lines on Lite.
VBUS already had its SMBJ22A (D1, 5.6 mm from the pin). Nothing protected
USB D+/D- (to the CH340B), CC1/CC2 (to the CH224A), the line-in RCA J2, the
sub-out RCA J3, or the four banana posts J4-J7.

**Lite stays top-side only.** Lite is the cheap tier, and JLC charges extra
for bottom-side assembly. The person chose top-only on 2026-10-31. The first
version of PR #146 put SOT-23 clamps on the bottom; this one doesn't. Every
clamp is a 0402-sized single-line part on F.Cu, wherever the top side has
room:

| Ref | Line | Track from pin | Ground |
|---|---|---|---|
| D3 | LINEIN_L (J2 pin 1) | 3.6 mm, no via | own via, 0.4 mm |
| D4 | LINEIN_R (J2 pin 3) | 3.0 mm, no via | own via, 0.4 mm |
| D5 | SUB_L (J3 pin 1) | 3.1 mm, no via | own via, 0.4 mm |
| D6 | SUB_R (J3 pin 3) | 3.6 mm, no via | own via, 0.4 mm |
| D7 | USB_DP (J1 A6/B6) | 9.4 mm, no via | own via, 0.3 mm |
| D8 | USB_DM (J1 A7/B7) | 13.8 mm, 2 vias | R1's GND pad / via, 1.1 mm |

All six are **PESDNC2FD5VB** (Prisemi, LCSC C110694, DFN1006-2L): bidirectional,
5 V working, 15 pF, rated to IEC 61000-4-2. That's $0.009 each, about $0.05 a
board, and one new JLC extended line.

- **RCA:** both signals are ground-centred, at most 2 Vrms (2.8 V peak).
  That fits inside 5 V either way. Leakage is at most 1 uA, next to the
  line-in's 15k and the sub-out's 1k. The clamps sit in the 1.3 mm gap
  between each jack's body and the parts below it. Each one's stub drops
  straight from the jack's pin.
- **USB:** 15 pF is fine for the CH340B's full-speed (12 Mb/s) link. D7 and
  D8 sit in the gap between J3 and J1, left of J1's body. That's where the
  pair leaves J1's pin field. D+ comes out on F.Cu, and D7 taps it where it
  turns down, with no via. D- dives to B.Cu at a via right at J1's pins, so D8
  reaches it with one more via (B.Cu stub at y 67.8). Nothing else is on
  either line before its clamp. The CH340B is far down the board.

**Exempt, with reasons** (`kicad/constraints.json` `esd`):

- **CC1/CC2.** J1's A5/B5 run about 2 mm of top track straight down to the
  CH224A (U1 pins 6/7). They pass between J1's VBUS pins, and U1 sits 1.4 mm
  below J1's body. No part fits on the top side ahead of U1. A clamp anywhere
  else would sit behind the CH224A pin, where the strike has already divided,
  so it would protect nothing (SLVA680 Fig 2-5). The CH224A's own pin
  protection has to take it. If Lite ever goes double-sided, the fix is one
  NUP2105L per line under J1's body. PR #146's first version did that, and
  core does it with D6/D9.
- **Speaker posts J4-J7.** Each leg ends on its LC filter's 1 uF X7R cap to
  GND (C68-C71). That cap sits about 16 mm of 1.2 mm track from the post, with
  the 10 uH inductor between it and the TPA3118. An 8 kV IEC 61000-4-2 contact
  strike carries 150 pF x 8 kV = 1.2 uC, which is about 1.2 V on 1 uF. The
  track's inductance drops the ns edge across itself, between the post and
  the cap, where nothing else connects. A TVS would add nothing. This is the
  same judgement as core's posts.

**Warnings left, by judgement** (`reports/check_esd.json`: 0 errors, 2
warnings, both on USB_DM): the 2 vias, and 13.8 mm of track before D8. That
is because D- leaves J1 on B.Cu, and the top side under J1's body can't take
a part. The inductance screen, which errs high, puts it at about 90 V on top
of the clamp voltage at 8 kV. The CH340B is still well behind the clamp.

**Also in this PR (hive-hwde #415 check_fp_attr):** L1-L4 (XR0850, two SMD
pads) carried `(attr through_hole)`, so bom-cpl sent them to JLC's THT step.
`fpfix.py --pcb` sets them to smd on the board, and `fpfix.py --lib` does
the same in the library footprint. bom-cpl now puts them on the SMT line.

**Silk.** D3-D8 are too small for a legible refdes, so theirs go to F.Fab.
R1's does too, because D8 now takes its spot. J1, J3 and C47 keep their
refdes, moved off the new pads (`kicad/gen/esd_silk.py`).

**How it was done** (reproducible, from `boards/lite-main`, starting from
the board on main):

1. Add the parts at the end of `kicad/gen/root.py`. Pull them with
   `hwde lib_pull --lcsc C110694 --project kicad`.
2. Run `sh build.sh sch` (with `HWDE=~/modules/bin/hwde`). Keep
   `kicad/lite-main.kicad_pro` as it was.
3. Run `hwde board_update --placements kicad/esd-placements.json`.
4. Under KiCad's python, run `kicad/gen/esd_eco.py kicad/lite-main.kicad_pcb`
   (stubs, ground vias, D8's via), then `kicad/gen/esd_silk.py`.
5. Run `fpfix.py --pcb kicad/lite-main.kicad_pcb --only
   IND-SMD_L8.0-W8.0_XR0850` (hwde scripts/lib).
6. Run `sh build.sh fab`. dfm passes with no waivers, and the CPL has no
   Bottom rows.

KiCad DRC with schematic parity is clean afterwards: 0 violations (warnings
included), 0 unconnected, 0 parity.
