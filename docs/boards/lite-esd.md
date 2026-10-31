# lite-main: ESD clamps at the connectors people touch (issue #133, Lite half)

**Finding.** hwde's `check_esd` (TI SLVA680: put the clamp first, right at the
connector, with a short path to ground) found 12 unclamped lines on Lite.
VBUS already had its SMBJ22A (D1, 5.6 mm from the pin). Nothing protected
USB D+/D- (to the CH340B), CC1/CC2 (to the CH224A), the line-in RCA J2, the
sub-out RCA J3, or the four banana posts J4-J7.

**Change: five clamps, all on B.Cu next to their connectors.** The top side
around J1, J2 and J3 is full. They are the same parts core uses
(docs/boards/core-esd.md):

| Ref | Part (LCSC) | Lines | Track from pin | Where |
|---|---|---|---|---|
| D3 | NUP2105LT1G (C14486), one channel | CC2 | 4.7 mm, 1 via | under J1's body |
| U10 | USBLC6-2SC6 (C2687116) | USB_DP, USB_DM | 8.0 / 6.9 mm, 1 via | under J1's body |
| D4 | NUP2105LT1G (C14486), one channel | CC1 | 7.1 mm, 1 via | right of J1 |
| D5 | PESD5V0S2BT (C49338) | LINEIN_L, LINEIN_R | 2.1 mm each | between J2's pins |
| D6 | PESD5V0S2BT (C49338) | SUB_L, SUB_R | 2.1 mm each | between J3's pins |

Each clamp's common pad has its own ground via (SLVA680 2.4). U10's ground
pad also runs straight into J1's shell pin 3. The ground pads of D5 and D6
also run into their jack's sleeve pin. Parts cost about $0.39 a board, plus
three new JLC extended lines. **The board now has parts on both sides.**
Lite used to be top-side only, so JLC will add its bottom-side assembly
charge. Look at that cost at order time.

- **CC:** NUP2105L: 24 V working voltage, 26.2 V minimum breakdown, ~30 pF.
  CC's 5.5 V stays inside it. A cable that shorts the 20 V PD VBUS onto CC
  will not make it conduct. In J1's pin field, B5 (CC2) can only get out
  upward and A5 (CC1) only up and to the right, so each line gets its own
  dual part. The unused channel has no net, like J1's A8/B8.
- **USB:** USBLC6-2SC6, I/O1 (pin 6) on D+ and I/O2 (pin 4) on D-. Their
  twin pads (1, 3) have no net. VBUS (pin 5) is left open: the board's VBUS
  is 20 V, above the part's 6 V rating, and +5V is far away. The internal
  zener still clamps through the steering diodes.
- **RCA:** both signals are ground-centred, at most 2 Vrms (2.8 V peak).
  That is inside the PESD5V0S2BT's bidirectional 5 V working voltage. Its
  leakage is in nA, next to the 15k line-in resistors and the sub-out's 1k.

**The USB-C rework.** Before this change, D+ left J1 on F.Cu and ran over
the top of B5 (CC2), so neither CC2 nor any clamp could reach the pins. The
2-3 mm of the USB pair under J1 is redone (`kicad/gen/esd_eco.py`):

- D+ drops to B.Cu at a via just above B6 and runs left at y 65.65 into the
  old D+ route. The old D+ via at 75.53/67.45 is gone, because that route is
  now B.Cu on both sides of it. A short F.Cu branch climbs to a via at U10
  pin 6.
- D- climbs from B7 to a via at 81.6/65.0 and runs left at y 65.0 into the
  old D- route at x 74.83. It taps U10 pin 4 on the way.

Past those points, the routes to the CH340B are unchanged.

**Exempt, with reasons** (`kicad/constraints.json` `esd`):

- **Speaker posts J4-J7.** Each leg ends on its LC filter's 1 uF X7R cap to
  GND (C68-C71). That cap sits about 16 mm of 1.2 mm track from the post,
  with the 10 uH inductor between it and the TPA3118. An 8 kV IEC
  61000-4-2 contact strike carries 150 pF x 8 kV = 1.2 uC, which is about
  1.2 V on 1 uF. The track's inductance drops the ns edge across itself,
  between the post and the cap, where nothing else is connected. The
  self-measurement taps (R66/R68, 100k into 470 pF) hang off the far side of
  the cap. A TVS would add nothing. This is the same judgement as core's
  posts.

**Warnings left, by judgement** (`reports/check_esd.json`, 0 errors):

- **One via between pin and clamp** on CC1, CC2, D+ and D-. The clamps can
  only sit on the bottom side. `check_esd`'s inductance screen, which it says
  errs high, adds 60-270 V on top of the clamp voltage at 8 kV. The most is
  on CC1, whose clamp D4 had to go outside J1's body.
- **CH224A pin before the clamp on CC1/CC2.** U1 sits 3-4 mm below J1 on
  F.Cu. Its pins are as close to the receptacle as the clamps are, so the
  strike divides between the clamp and the CH224A's own pin protection. A
  layout that put the clamp first would mean moving U1, which is beyond a
  clamp ECO.

**Order-time check.** hwde does not model JLC's bottom-side placement, so the
five bottom parts have dfm waivers (`reports/dfm-waivers.json`). For the
SOT-23 parts, only pin 3 has to land on the lone GND pad. U10 must not be
turned 180 degrees: that would swap GND (pin 2) and VBUS (pin 5). Confirm
pin 1 in JLC's bottom-side preview.

**How it was done** (reproducible, from `boards/lite-main`):

1. Add the parts at the end of `kicad/gen/root.py` and pull them with
   `hwde lib_pull --lcsc C14486 C49338 C2687116 --project kicad`.
2. Run `sh build.sh sch` (with `HWDE=~/modules/bin/hwde`). Keep
   `kicad/lite-main.kicad_pro` as it was.
3. Run `hwde board_update --placements kicad/esd-placements.json`.
4. Run `kicad/gen/esd_eco.py kicad/lite-main.kicad_pcb` under KiCad's
   python. It moves the USB pair under J1 and adds the stubs and ground
   vias.
5. Run `sh build.sh fab`, then the dfm gate with
   `--waivers reports/dfm-waivers.json`.

KiCad DRC with schematic parity is clean afterwards: 0 violations,
0 unconnected, 0 parity.
