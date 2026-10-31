# core: ESD clamps at the connectors people touch (issue #133)

**Finding.** hwde's `check_esd` (TI SLVA680: clamp first, right at the
connector, short to ground) found 18 unclamped lines on core. VBUS had its
SMBJ30A (D1, 5 mm from the pin) and USB D+/D- had the USBLC6 (U2, about 20 mm
away). Nothing protected CC1/CC2, the sub-out RCA, the line-in RCA, the four
speaker posts or the Ethernet jack.

**Change: four clamps, all on B.Cu next to their connectors** (the top side
there is full):

| Ref | Part (LCSC) | Lines | Track from pin |
|---|---|---|---|
| D6 | NUP2105LT1G (C14486), one channel used | CC1 | 7.8 mm |
| D9 | NUP2105LT1G (C14486), one channel used | CC2 | 1.2 mm |
| D7 | PESD5V0S2BT (C49338) | SUB1_OUT, SUB2_OUT | 3.3 mm each |
| D8 | PESD5V0S2BT (C49338) | LIN_L, LIN_R | 6.0 / 1.5 mm |

Each clamp's common pad has its own ground via (SLVA680 2.4). There is no
via between any pin and its clamp. `check_esd`'s inductance screen, which it
says errs high, adds 0 to 124 V of overshoot on top of the clamp voltage at
an 8 kV strike. The most is on D7's 3.3 mm stubs; the line-in and CC lines
are at 0-55 V. Cost: four parts, about $0.34 a board, and
two new JLC extended lines.

- **CC:** NUP2105L has a 24 V working voltage and breaks down at 26.2 V min,
  well above CC's 5.5 V. Its ~30 pF is far under the 620 pF a PD BMC receiver
  tolerates. A cable that shorts 28 V EPR VBUS onto CC would make it conduct.
  The CH224Q's own CC pin faces that same fault anyway. J1's two pin rows
  leave A5 (CC1) reachable only from the right and B5 (CC2) only from the
  left, so each CC line gets its own dual part. The unused channel has no net,
  like J1's A8/B8. D6's pad sits on the CC1 run just past the pin field.
- **RCA:** both signals are ground-centred, at most 2 Vrms (2.8 V peak). The
  PESD5V0S2BT is bidirectional with a 5 V working voltage, so the signal stays
  inside it. Its leakage is nA, against the 470 R and 4.7 k it sits beside.

**Exempt, with reasons** (`kicad/constraints.json` `esd.exempt_nets` and
`exempt_notes`):

- **Speaker outputs, J6/J7/J10/J11.** Each leg's 1 uF filter cap, plus the
  470 nF Zobel, sits about 6 mm from the post, and the 10 uH inductor stands
  between it and the TPA3220. An 8 kV IEC contact strike carries 150 pF x 8 kV
  = 1.2 uC, which is about 0.8 V on 1.47 uF. The inductor blocks the ns edge
  from the amp. A TVS would add nothing.
- **Ethernet, J2.** The HR911105A is a MagJack. The cable ends on its
  transformers and Bob Smith termination (1500 Vrms isolation), and these
  pins are on the PHY side. +3V3E is the centre-tap supply.

**Warnings left, by judgement** (`reports/check_esd.json`):

- **USB D+/D- clamp far (U2, 19-23 mm).** Moving U2 to J1 would be a re-route
  of the USB pair through the VBUS via field. USB on core is for firmware
  flashing at the bench, not a port a listener plugs into, and the line still
  is clamped. Left for the next layout touch.

**How it was done** (reproducible, from `boards/core`):

1. Edit `kicad/gen/blocks.py` and `blocks_audio.py`, and pull the parts with
   `hwde lib_pull --lcsc C14486 C49338`.
2. Rebuild with `kicad/gen/build.py`. Keep only power_in, dac and linein.
   Add the two `parts/*.json` entries by hand (see `~/kb/hifi-receiver.md`
   on build drift).
3. Export the netlist, then run `hwde board_update --placements
   kicad/gen/esd_placements.json`. The spots for D6, D7 and D9 come from
   `kicad/gen/esd_spots.py`. D8 is placed by hand, as `esd_eco.py` explains.
4. Run `python3.11 kicad/gen/esd_eco.py`, which adds the stubs and ground vias
   and puts the router's pad-centre ends back on the 45 degree grid.

KiCad DRC with schematic parity is clean afterwards. `check_esd` reports 0
errors and the 2 USB warnings above.
