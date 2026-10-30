# Review round 3 (Lite): BAN, the banana-socket change (d1543da)

Scope: what commit d1543da changed on `boards/lite-main`, as it is in the working tree, which also
carries the round-2 sub mute re-applied after the merge. That covers:

- J4-J7 (Amass 24.243.1/.2, C7437321/C7437322);
- the 118 mm board, and L1-L4 moved 2.2 mm toward U8;
- C68-C71 and the R66/R68 taps;
- the speaker copper (`kicad/gen/banana_route.py`);
- the rear wall (`enclosure/lite.py`, `core_box.py`).

Read-only. hwde ran on `/tmp` copies. Tests are in `docs/review-4/sims/ban/`, with outputs in `out/`.

**Result: 1 blocker, 0 major, 4 minor, 2 notes.**

| id | sev | repeat | one line |
|---|---|---|---|
| R4-BAN-1 | **blocker** | new | The box's 6.8 mm square holes at 7.5 mm can't pass the socket's ~9.5 mm round collar at 8.1 mm, so the board can't be fitted. Fix: 10.2 mm holes (or one 44 x 10.2 slot) at 8.1 mm in core_box.py |
| R4-BAN-2 | minor | new | The new TAP_R run under L1/L2 cuts the left filter caps' F.Cu GND return: C68/C69 to U8 is 59/67 mm (was 42/38, 22/19 without the cut). Fix: re-route TAP_R |
| R4-BAN-3 | minor | new | GND pour 0.127 mm from all 8 socket THT rings, both sides: a solder-bridge risk at wave/hand soldering. Fix: 0.3 mm clearance class for /SPK_* |
| R4-BAN-4 | minor | new | 11 mm pitch: metal-shell plugs wider than 11 mm collide or short (J5/J6 are two channels). The user manual tells Lite owners they can use bare wire, spades or dual plugs. Fix: docs, plug-size limit |
| R4-BAN-5 | minor | new | Symmetric 2-pin THT with its orientation silk off the board: JLC can fit it back to front. Overhang of 2.65 mm with no order note. Fix: silk arrow, order remark |
| R4-BAN-6 | note | new | Footprint, pinout, polarity, rating and speaker-copper current all pass (1.25 mm vs 1.08 needed at 3.2 A rms; 7 vias per N net) |
| R4-BAN-7 | note | R3-LAYOUT-11 | OUT_x fan-out is still 0.31 mm for ~6 mm, budgeted at 1 A. Not touched by this change |

## Findings

### R4-BAN-1 (blocker, new): the rear-wall holes are smaller than the socket's collar

**What the enclosure assumes.** `core_box.py` uses BANANA_NOSE 6.0 ("square nose"), BANANA_AXIS 7.5 and BANANA_HOLE 6.8. The 6 mm is the EasyEDA footprint's silk tab (local y ±3.0), which is a drafting mark, not the part's size.

**What the part is.** Amass's drawing (LCSC datasheet for C7437321, "Product Specification" VER 2.0) is vector. `amass_nose.py` measures it, scaled from the 24 mm overall length:

- body 13.00 mm tall (the drawing's 19 − 6);
- collar at the plug end 9.49 mm tall and 2.51 mm long, its bottom 3.35 mm over the board;
- bore axis 8.11 mm over the board, bore 4.06 mm;
- pins 1.20 mm at a 15.77 mm pitch.

LCSC's photos of both colours show the collar is a cylinder, so it is ~9.5 mm across in plan as well.

**What happens.** The collar spans 3.4–12.9 mm over the board; the hole spans 4.1–10.9 mm. The collar sticks 2.65 mm past the board edge, so it reaches 2.05 mm into the 2.4 mm wall:

- the board cannot drop onto its standoffs;
- it ends up ~2 mm forward;
- the M3 screws miss the pillars.

mech's fit check passes `port_J4..J7` only because its model of the socket has the 6 mm nose.

**Fix (enclosure only, no board change).**

1. In `core_box.py`, set BANANA_NOSE 9.6 (round), BANANA_AXIS 8.1 and BANANA_HOLE 10.2.
2. Holes: either round holes with a teardrop roof, or one 44 × 10.2 mm slot across J4–J7.
   - Separate holes at 11 mm pitch leave a 0.8 mm web, the PETG minimum.
   - With the slot, the 10.1 × 13 mm body faces bear on the wall above and below it, which takes the plug-extraction load.
3. Model a cylinder in `banana_ports()`, then re-run `mech build`/fit.
4. Caliper one sample before printing.

### R4-BAN-2 (minor, new): TAP_R splits the left channel's filter-cap return

**The cut.** `banana_route.py` runs TAP_R from R68 at y 89.52, x 86.3–102.5, along the L1/L2 centre line on F.Cu. That is the F.Cu GND under L1/L2. B.Cu under the whole L/C row is the PVDD pour, and the change removed the old C68 GND via.

**What it costs.** `cap_return.py` finds the shortest GND path from each cap's GND pad to a U8 GND pin (grid Dijkstra over both layers' fills, joined at vias):

| cap | now | before d1543da | now, TAP_R run deleted |
|---|---|---|---|
| C68 | 58.7 mm (via at (84,80), B.Cu round the PVDD pour) | 41.9 | 22.1 |
| C69 | 67.2 | 38.2 | 19.4 |
| C70 | 22.0 | 26.0 | 22.0 |
| C71 | 23.8 | 33.8 | 23.8 |

This loop carries the BD-mode common-mode ripple: ~1.3 A p-p per output at 400 kHz (21 V × 0.25 / (10 µH × 400 kHz)). The cost is EMI only. The filter still works: 60 mm is ~60 nH, so C68's self-resonance is ~650 kHz, and attenuation above it is still L/Lpar ≈ 44 dB. This is the same family as R3-AMP-5. TAP_R also picks up the left inductors' field into the right tap, but the firmware does not read the taps.

**Fix.** Re-route TAP_R so that it does not divide that pour:

- on B.Cu just inside the PVDD pour's top edge (y ≈ 81.8); or
- north of the caps; or
- DNP R68/R69 and drop the track, since HIFI_HAS_TAPS is 0.

Target: C68/C69 ≤ 30 mm in `cap_return.py`.

### R4-BAN-3 (minor, new): pour clearance 0.127 mm at the socket rings

**Where.** All 8 J4–J7 rings (3.0 mm pad, 2.0 mm drill) are 0.13 mm from the GND pour on F.Cu and on B.Cu. The 1.25 mm speaker tracks are 0.122–0.127 mm from the GND pour and from the B.Cu PVDD pour.

**Electrically fine.** At ≤ 21 V this meets IPC-2221B (B2 0.1 mm, B4 0.13 mm).

**The assembly risk.**

- These are hand- or wave-soldered THT joints.
- With ~0.05 mm mask expansion, the mask web between ring and pour is ~0.08 mm, under JLC's 0.1 mm minimum web.
- A bridge shorts an output to GND. The TPA3118 latches off, so nothing is damaged, but there is no sound until the joint is reworked.

The old 4-pin terminal had the same 0.127 mm, so the pour clearance is not new; the rings are.

**Fix.** A netclass or custom rule giving /SPK_* 0.3 mm to zones, then refill. No re-route is needed.

### R4-BAN-4 (minor, new): plug pitch and the docs

**Plug pitch.** The pitch is 11.00 mm and the collars are 1.5 mm apart.

- Banana plugs wider than 11 mm collide.
- Metal-shell audio plugs, whose shell is the conductor, can touch.
- J5 (L−) and J6 (R+) are on different channels. Either short trips the TPA3118's over-current protection: it mutes, without damage.

**The docs.**

- `docs/manual/user-manual.md` (Speakers → Terminals, lines 120–123) tells every tier "4 mm binding posts … single, dual, spades, or bare wire under the nut". The quick start is right for Lite; the manual is not.
- A Lite owner with bare wire needs four plugs, which aren't in "What else it needs".

**Fix.**

- Docs: in the manual, give Lite its own line: "single 4 mm banana plugs no wider than 10.5 mm, or with an insulated shell; no bare wire, spades or dual plugs". Add 4 plugs to the Lite box's "What else it needs".
- Next spin: 13 mm pitch within a pair and 15 mm between pairs, if the edge allows.

### R4-BAN-5 (minor, new): fitting orientation at JLC

**Nothing keys the socket.** The two pads are identical and sit at ±7.85 mm. A socket turned 180° inserts and solders, with its collar facing into the board. The cpl_pin1 check cannot see this, because the pads are symmetric.

**The orientation silk is off the board.** The footprint's only orientation mark, the nose tab, lies past the rear edge, so it isn't printed. What is left is the pin-1 dot at the back corner and a 1.2 mm asymmetry in the body outline.

**The overhang is undeclared.** The 2.65 mm overhang has no order remark and no rail choice.

**Fix.** Add a silk arrow and the text "PLUG →" inside the outline at each socket's rear. Add an order remark: "J4–J7 collar to rear edge, overhangs 2.65 mm". Choose rails on the left, right and front edges only, or none.

### R4-BAN-6 (note): what passes, with margins

| block | reference | ours | margin |
|---|---|---|---|
| Pin pitch | Amass drawing 15.7 (measured 15.77) | 15.70 | 0.07 |
| Pin positions | 3.41 mm from the back face, 4.82 mm from the nose face | 3.50, 4.80 | ≤ 0.09 |
| Hole | 1.2 mm blade, diagonal ~1.3 | 2.0 drill, 3.0 pad (0.5 ring) | 0.35 mm radial |
| Contact pin | brass contact and its tail at the back end (section) | both routes land on pad 1 | pin 2's connection irrelevant |
| Polarity | .1 red, .2 black (LCSC photos) | J4/J6 red on SPK_x_P, J5/J7 black on SPK_x_N | ok |
| Rating | 24 A, 60 V DC, ≤ 5 mΩ | 21 V, 4.6 A peak; 31 mW per socket at 2.5 A | wide |
| SPK copper | IPC-2152 +30 °C: 0.825 mm at 2.5 A rms (3.5 A peak sine); 1.084 mm at 3.2 A rms (4 Ω full power, no governor) | 1.25 mm everywhere | +15 % at 3.2 A |
| N-net layer change | ~1 A per 0.4 mm via | 7 vias | 2.8× at 2.5 A |
| Spacing between speaker nets | IPC B2 0.1 mm at ≤ 30 V | min 0.475 mm (SPK_L_N–SPK_R_P) | 4.7× |
| L to C | TI SLOS708G §10.1: filter close to the device | L pad 2 to C pad 1 3.1 mm; L row now 8 mm from U8's output pins; pin to cap 22–28 mm | ok |
| GND islands under the sockets | none | 1 group (gnd_islands.py) | ok |
| Socket body vs C68–C71 | courtyard | 0.3 mm (cap pad 1) to 1.2 mm (GND pad) | ok |

The tap dividers' new places: R66 (100k) sits on SPK_L_P by C68; R68 sits on SPK_R_P between J4's pins and C70. Their 10k halves (R67/R69) stay by the codec. Placement is fine; only TAP_R's route is a problem (R4-BAN-2).

### R4-BAN-7 (note, repeat of R3-LAYOUT-11)

The OUT_x fan-out from U8 to the bootstrap caps is still 0.308 mm wide and 5.6–6.6 mm long. It needs 0.825 mm at 2.5 A rms. The constraints still budget OUT at 1.0 A, so hwde passes it. The new L pad-1 links are 0.8 mm, at the limit.

## Field issues

- **Loosening:** immune. There is no nut; the plug's spring is the wearing part.
- **Bare-wire strands:** possible. Strands can bridge the 1.5 mm between collars. The TPA3118 latches off, so nothing is damaged. See the R4-BAN-4 docs fix.
- **Spade lugs:** not applicable; the sockets don't take spades.
- **Metal-shell plugs touching:** possible above 11 mm across (R4-BAN-4).
- **Socket fitted back to front:** a JLC risk (R4-BAN-5).
- **Collar clashing with the case:** yes (R4-BAN-1).

## To check on a sample before ordering (known unknowns)

1. Collar diameter and axis height (R4-BAN-1).
2. How deep the brass contact starts. The drawing's broken-out section shows brass only in the last ~7 mm, 17–24 mm from the plug face. Check with a short (15 mm-pin) audio plug and a 19 mm plug, both fully home with the box on: the meter should read < 10 mΩ. If short plugs miss, the manual must say "≥ 18 mm pins".
3. Whether pin 2 is connected to the contact. This doesn't matter as routed.
4. That JLC accepts the overhang with the chosen rails.

## Tests

| test | what it checks | result |
|---|---|---|
| `sims/ban/amass_nose.py /tmp/ds3/ban/amass243/d.svg` | drawing vs the enclosure constants (its header says how to fetch the drawing) | FAIL (R4-BAN-1) |
| `sims/ban/ban_geom.py <refilled copy>` | sockets, copper gaps, widths, vias, L–C, cap GND vias | FAIL (R4-BAN-3 gaps; caps' GND vias 3.7–9.0 mm) |
| `sims/ban/cap_return.py <refilled copy> [-v]` | filter-cap GND return, ≤ 30 mm | FAIL C68/C69 (R4-BAN-2); passes with TAP_R deleted |
| `sims/ban/spk_current.py` | hwde check_current at 2.5 A and 3.2 A rms on a /tmp copy | PASS (SPK); OUT under (R4-BAN-7) |
