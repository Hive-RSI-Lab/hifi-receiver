# Review round 4 (docs/review-5): ultra-front, full review, and its interface to core

Reviewer UF. Board: `boards/ultra-front` (first full review, all domains), the
FFC link to core J3, the firmware that drives the front board, and issue #108's
three core facts (J3 vs ultra-front, TOSLINK, USB4085). Read-only; tests in
`docs/review-5/sims/uf/`.

**Result: 0 blockers, 1 major, 5 minors, 9 notes.** The one major is firmware
only (one line) and is shared with Plus: the knob turns the volume the wrong
way. The hardware has no blocker or major. The FFC interface checks pin for pin,
and the cable is type B. The fab package matches the PCB.

## Findings

### R5-UF-1 (major, both): clockwise on the knob turns the volume down

- **The part:** the EC11E1834403 has no detents and 18 pulses a turn. Source:
  the Alps EC11 catalogue (LCSC C361165 copy), product table p.1 row
  EC11E1834403: torque 7 +3/-4 mN.m, detents "无" (none), 18 pulses, push
  switch.
- **What the datasheet says:** Alps p.282 "output waveform" (EC11E 18-pulse
  panel), CW direction: A changes a quarter cycle before B. A leads.
- **How it reaches the decoder:**
  - The pull-ups make OFF (open) read 1. Read as (A,B), clockwise from rest is
    11 -> 01 -> 00 -> 10 -> 11.
  - Pad A is Alps terminal A. The footprint SW-TH_EC11E1820402 at 180 deg puts
    A top-right, B top-left, D bottom-right and E bottom-left. That is Alps
    fig. 5 (seen from the insertion side) turned 180 deg, not mirrored.
  - A goes to ENC_A, then core U17 P12, then bit 10, which is `enc_a`.
- **What the firmware does:** `hifi_exp.c` QUAD counts that sequence as -1 an
  edge. `test_core.c` assumes the opposite ("one detent clockwise from rest:
  11 -> 10 -> 00 -> 01 -> 11", which has B leading). `hifi_vol.h`'s "+
  clockwise" means volume up.
- **Effect:** one clockwise turn gives -18 steps, which is -36 volume points
  (`HIFI_VOL_KNOB_STEP` 2). Turning right makes it quieter.
- **Plus too:** core SW1 has the same footprint, rotation and A/B nets.
- **Test:** `sims/uf/enc_rev.c`, which fails with "clockwise (A leads) counts
  -18 per turn".
- **Fix (firmware):** in `board_core.c` `controls()`, swap to
  `.enc_a = EXP_ENC_B, .enc_b = EXP_ENC_A`. Correct `test_core.c`'s cw/ccw
  tables to A-leads.
- **Bench check:** one clockwise turn should raise the volume by 36. This is
  the only unverified link: Alps' CW is by industry convention seen from the
  shaft end, but the catalogue does not say so.
- **Other knob points:** `edges_per_detent` = 4 gives one step per pulse, so
  18 steps a turn and 2.8 turns from silent to full. That is right for a knob
  with no detents. The README's "the knob has no detents" is correct. If the
  detented EC11E18244A5 named in the README is swapped in, it needs `= 2`.
  Chatter adds nothing (enc_rev.c check 2).

### R5-UF-2 (minor, ultra-front): user-touched switches have no clamp, and their metal reaches the expander pins

- **The buttons:**
  - The TS-1187A-B-A-B has a **brass** keystake (actuator) and a stainless
    cover. XKB drawing TS-1187A-X-X-X: "Keystake A: PA46, B: Brass".
  - BTN1-3 run from the switch to U1 P03-P05 with only a 10 k pull-up. There
    is no series R, no capacitor and no TVS.
  - The way in from the panel face: the printed cap's 0.3 mm annular gap
    through the 2.4 mm wall, then the 0.05 mm gap past the 8.5 mm flange (0.95
    + 0.75 mm), then 0.3 mm of air to the switch top. That is about 5.8 mm in
    all (enclosure/core_box.py `ultra_buttons`, WALL, BTN_*).
  - An 8 kV IEC air discharge creeps about 1 mm per kV along seams. A strike
    can therefore reach the switch metal. Whether that metal is on BTNx or on
    GND depends on which terminal pair carries the dome, which the drawing
    does not give.
  - The UMW TCA9535 datasheet gives no ESD rating. TI's is 2 kV HBM.
- **The knob:** a strike lands on the EC11 frame, F/G, which is GND on both
  pours. That is safe locally. The return to core is through 5 FFC GND
  conductors, about 15 nH together. If 10 % of a 30 A, 0.8 ns strike crosses,
  the two boards' grounds move about 55 V apart for a nanosecond. That is a
  screen-level estimate, untested.
- **Fix:**
  - Cheap: a PESD5V0S2BT (C49338, already on core) on BTN1/BTN2 and on
    BTN3/ENC_SW, at the switches with their own GND via, at about $0.05 each.
  - Or 1 nF X7R from each BTN net to GND at its switch.
  - Or a plastic-keystake variant, if JLC stocks one.
  - At bring-up, gun-test 8 kV air on each button cap and on the knob.

### R5-UF-3 (minor, interface): SPI at 20 MHz against the ST7789's 66 ns write cycle; R402/R403 are not at the source

- **The clock:** `CONFIG_DISPLAY_CONFIG speed=20000000` gives a 50 ns period.
  The ST7789 serial write cycle is 66 ns minimum (15.15 MHz), and
  requirements.md and the README both say "15 MHz or less". The clock runs
  32 % over spec.
- **The terminators:** R402/R403 (33 R) sit 44-49 mm of track from U7's pins.
  The README asked for them at the ESP32.
- **Simulation:** `sims/uf/tb_sck_asbuilt.cir`, a lossless line chain with the
  routed lengths, a 50 mm FFC at 100 ohm, a fast 25 ohm / 1 ns driver and an
  8 pF load. It passes:
  - the high level stays at 3.25 V or more (VIH 2.31 V, so 0.94 V margin);
  - the low level stays at 0.05 V or less (VIL 0.99 V);
  - overshoot reaches 3.77 V and undershoot -0.47 V.

  So signal integrity is not the risk. The risk is the panel's timing at 20 MHz.
  ST7789 panels run at 27-80 MHz in the field (TFT_eSPI defaults), so it most
  likely works.
- **Fix:** speed=16000000 in the HIFI_ULTRA DISPLAY_CONFIG default. That costs
  about 25 % redraw time and puts the part in spec. Alternatively, keep 20 MHz
  and check a full-screen redraw at bring-up.

### R5-UF-4 (minor, interface): the backlight is on/off only, though the requirements and product.md say PWM-dimmable

- **Hardware:** LCD_BL is core U17 (TCA9539) P06, an I2C expander bit, not a
  PWM-capable GPIO. The front board's names (LCD_BL_PWM, B3 "PWM-dimmable from
  the core") and product.md l.462 ("backlight PWM") don't match the wiring.
- **Effect:** the backlight runs at a fixed 39-64 mA, against the module's
  80 mA maximum. Firmware keeps it at full brightness all through playback
  (`exp_set(EXP_LCD_BL, audio_active || touched < 60 s)`). In a dark room that
  is a bright 2" panel for the whole session.
- **Fix:** accept it and correct the docs. Or add firmware software PWM through
  the expander: 200 Hz takes 400 writes a second at about 75 us each, which is
  3 % of the 400 kHz bus. A hardware PWM needs a free ESP32 GPIO, and there is
  none.

### R5-UF-5 (minor, interface): the FFC cable isn't pinned down as a buyable part, and the wrong one makes outputs fight

- **What the docs say:**
  - enclosure/README.md and the ultra-front README: 20-pin, 0.5 mm, type B,
    30-50 mm.
  - `docs/bom-estimate.csv` l.46: "FFC cable 20P 0.5 mm 100 mm", with no type
    or part number.
- **The common default is type A.** A type-A cable gives no contact as
  inserted. With one end flipped, it maps pin N to pin 21-N
  (`sims/uf/ffc_orient.py`):
  - rails still meet rails;
  - core LCD_BL (U17 push-pull output) meets front GND on pin 11;
  - core FFC_SCK (GPIO32 output) meets the front's ENC_A, which is GND
    whenever the knob rests with A closed.

  That is an output shorted to GND for as long as it drives high: 40-80 mA
  from GPIO32, and the TCA9539's 50 mA absolute maximum per pin.
- **Fix:** give the order list a concrete LCSC/AliExpress part, "0.5 mm 20P
  FFC, 50 mm, type B / reverse". Correct bom-estimate's 100 mm to 30-50 mm.
  The route needs about 25-30 mm: 4.1 mm J3 mouth to the board back, about
  4 mm down, 1.6 mm under the board, 1.8 mm up, plus 2 x 3 mm inserted.

### R5-UF-6 (minor, ultra-front): HASL on 0.3 mm FPC pads

- The gbrjob says `"Finish": "HASL"`. J1/J2 are 0.5 mm pitch with 0.3 mm pads,
  as are U1's 0.4 mm TSSOP pads.
- Round 1's DFM-8 disposition ("order ENIG", `docs/review-2/dispositions.md`
  l.44) names Lite and core only.
- **Fix:** order ENIG for ultra-front too, about $2-5 a 5-board order. Or add
  the board to the DFM-8 order note.

### Notes

- **R5-UF-N1:** the IR supply filter sits away from the receiver. R10/C5 are
  9.6 mm of IR_VCC track from U2 pin 4, against Everlight DMO-0000008's "as
  close as possible". There is little switching noise on this board: the
  backlight is DC, and SPI and I2C are far above the 38 kHz band. Bench: range
  test with the panel redrawing.
- **R5-UF-N2:** the LCD runs slightly over its operating maximum.
  HS20HS072RX VCI/IOVCC operating max is 3.3 V, and core +3V3 is 3.33 V
  nominal (R15/R16 0.6 x (1+100k/22k)), so about 3.40 V at the 1 % top. The
  absolute maximum is 4.6 V, a 1.2 V margin. Accept.
- **R5-UF-N3:** the EC11 footprint centres its two pin rows. They sit at 7.25 mm
  where Alps has 7.5 (A C B) and 7.0 (D E), so the shaft is 0.25 mm off the
  footprint origin, which the enclosure uses for the knob. The 1.2 mm holes
  take it, and the 22 mm knob clears the panel by 1 mm (FKNOB_LIFT). Known from
  round 1 (DFM-14).
- **R5-UF-N4:** U1 is the UMW TCA9535PWR clone (C22396388). Its datasheet
  matches TI's: registers, 0x20-0x27, open-drain INT, inputs to 5.5 V above VCC
  (UMW p.6 "may be raised above VCC to a maximum of 5.5V"). It has no ESD
  figure.
- **R5-UF-N5:** EXP_INT is on GPIO39 (SENSOR_VN). ESP32 errata 3.11: an
  about 80 ns low glitch when the SAR ADC powers up. ISENSE reads ADC1, so
  expect spurious notifications. They cost one extra 4-register I2C read,
  about 0.4 ms. Harmless.
- **R5-UF-N6:** `exp_init` turns the backlight on before squeezelite's
  display_init and `hifi_display_reset`'s 130 ms, so the panel flashes white
  or garbage at boot. Cosmetic. Turning LCD_BL on after display init fixes it.
- **R5-UF-N7:** firmware selects `rotate` with a 320x240 panel. Whether the
  image lands upright with the tail to the left (J2) can only be confirmed on
  the bench.
- **R5-UF-N8:** a reversed rail order is safe. Pins 1/20, 2/19 and 3/18 mirror
  (ffc_orient.py), as claimed.
- **R5-UF-N9:** the U2 EasyEDA model's silk dot sits nearest pad 4
  (`cpl_pin1.py` CHECK). The pinout is right regardless:
  - Everlight's 3D view has 1-2 on one side and 3-4 on the other, with 1
    opposite 4, numbered counter-clockwise.
  - The footprint has 1 (GND) top-left, 2 (GND) bottom-left, 3 (OUT)
    bottom-right and 4 (VCC) top-right, counter-clockwise in the top view.
  - The model pads land on the board pads with the same numbers.

## The interface to core, block by block

### FFC J3 (core) <-> J1 (front), from `sims/uf/ffc_orient.py` (PASS)

- **Parts and pins:** both ends are XUNPU FPC-05F-20PH20 (C2856803), at rot 0
  on F.Cu. All 22 pads carry the same signal, after core's names FFC_SCK,
  FFC_MOSI, LCD_BL, EXP_INT and IR_IN map to LCD_SCK, LCD_MOSI, LCD_BL_PWM,
  FRONT_INT_N and IR_OUT.
- **Mouth:** the XUNPU drawing was repaired from LCSC's corrupt PDF by cutting
  the broken incremental update. It shows the terminal tails on the far side
  from the actuator, with the hold-down tabs on the cable side. In both
  footprints the tabs (pads 21/22) sit at +1.29 mm and the pins at -1.29 mm,
  so the mouths face +y:
  - J3's mouth faces core's front edge, its centre 3.50 mm from it;
  - J1's mouth faces the front board's bottom edge, 4.40 mm from it.
- **Conductor order:** J3 pin 1 is at box x 15.25. J1 pin 1 is at front X 9.25
  + FB_X 6.0 = 15.25. All three bends are about the box x axis, so pin N meets
  pin N.
- **Contact face:** the cable leaves J3 contacts-down (bottom contact). After
  turning down, forward and up, those contacts face forward, the panel side.
  J1 sits on the front face with its contacts facing rearward, into the board.
  The far end therefore needs opposite-side contacts: **type B**, as
  enclosure/README.md says. The round-1 known-unknown (parts:64) holds.
- **Contact rating:** 0.5 A per contact (XUNPU spec "Rated Current 0.5A").

### Signal levels

All signals are 3.3 V CMOS at both ends.

| Signal | Source -> load | Check |
|---|---|---|
| SCK/MOSI/DC | ESP32 GPIO32/33/15 -> ST7789 | VIH 0.7 x IOVCC = 2.31 V; SI bench passes (R5-UF-3) |
| LCD_CS | core R57 10 k to GND | held asserted; firmware cs=-1 with **mode=3**, the setting a CS-less ST7789 needs |
| LCD_BL | U17 P06 push-pull -> R12 100 R -> AO3400A gate, R11 100 k pull-down | Vgs 3.3 V against Vth max 1.45 V; off through any expander reset (TCA9539 has no pull-ups) |
| INT | TCA9539 + TCA9535 open-drain, wired-OR; R36 10 k (core) // R7 10 k (front) = 5 k | 0.66 mA sink against 3 mA at 0.4 V |
| IR | IRM-H638T OUT -> GPIO35; R58 10 k // R9 10 k = 5 k | 0.66 mA against VL 0.5 V at 2 mA (DMO-0000008) |
| ENC_A/B | EC11 -> both expanders; R52 // R1, R53 // R2 = 5 k; C37 + C6, C38 + C7 = 20 nF | tau 100 us against Alps' 3 ms chatter; 18 pulses x 4 edges x 5 rev/s = 360 edges/s, 2.8 ms an edge, against about 0.2 ms to read both expanders |

### I2C

Re-ran `docs/review-3/sims/digital/i2c_bus.py`: pass.

- **Bus loading:** with a 150 mm FFC (pessimistic), core plus front measure
  217 + 53 mm (SCL), 78 pF and t_r 146 ns, against 300 ns at 400 kHz (the
  firmware's speed). That is a 2.1x margin. Isink is 1.32 mA against 3 mA.
- **Pull-ups:** 2.2 k on core only (R34/R35), none on the front, as both
  READMEs say.
- **Addresses:** CH224Q 0x22, ES7243E 0x10 (or 0x14), ES9018K2M 0x48, TCA9539
  0x74 and TCA9535 0x27 (A0-A2 = +3V3, checked on the netlist). All are unique.
  The firmware's TCA9539 probe list (0x20, 0x74-0x77) can't pick up 0x27.

### Power over the FFC

- **+5V (pins 3, 18):** worst case 67 mA, from the backlight at 5.25 V with Vf
  2.7 V, (5.25-2.7)/40 R = 64 mA, plus one LED colour at about 3.4 mA. The
  firmware lights one colour at a time. That is 34 mA a contact against
  0.5 A, a 15x margin.
- **+3V3 (pins 2, 19):** 15 mA or less: IRM 0.7 mA max, every pull-up held low
  2.3 mA, TCA9535 5 uA, and the ST7789 logic (module DS "TBD"; assumed 10 mA
  or less).
- **Drop:** about 0.2 ohm a contact pair (2 x 60 mOhm contact, XUNPU 4.3, plus
  about 80 mOhm of 50 mm conductor). +5V drops 7 mV and GND rises 3 mV, both
  negligible.
- **Backlight current over the corners:** 39 mA (4.75 V, Vf 3.2 V) to 64 mA,
  against the HS20HS072RX's 80 mA. Each 120 R dissipates 55 mW or less,
  against 100 mW.

### Firmware for the front board (round 2's R3-DIGITAL-3 fix)

These parts are correct:

- `hifi_display_reset` pulses LCD_RST low for 10 ms, then waits 120 ms. The
  patch calls it at the top of display_init.
- `front_init` probes 0x27, sets port 0 to inputs, and shows blue.
- Each loop reads both ports of both expanders, which clears the wired-OR INT,
  with a 10 ms timeout as a backstop.
- The buttons map from front P02-P05 to bits 12-15.
- RGB: red off is set as an input. Green and blue off drive high at 3.3 V,
  giving 2.05 V across the LED at worst, against InGaN Vf 2.8 V min at 20 mA,
  so they stay dark.
- `SET_GPIO 35=ir:nec`; `SPI_CONFIG data=33,clk=32,dc=15` matches core U7.

These parts are wrong or loose: the knob direction (R5-UF-1), the SPI speed
(R5-UF-3) and the backlight flash (N6).

## ultra-front board, block by block

| Block | Reference / rule | Ours | Margin |
|---|---|---|---|
| U1 TCA9535 address, INT | UMW/TI DS | A0-A2 high = 0x27; INT 10 k | unique; 0.66 mA |
| U1 unused pins | no floating inputs (INT can't be masked) | P06/P07/P14-P17 inputs, shared R19 10 k to GND | firmware keeps them inputs (cfg 0xF0 upper) |
| U1 decoupling | 100 nF at VCC | C1 about 3 mm from pin 24 | ok |
| RGB D1 (TJ-S1615) pins | Togialed DS p.2 top view: 1 top-right common anode, 2 B, 3 G, 4 R | footprint the same | ok |
| RGB currents | 30 mA abs max | R ~3.1 mA (1 k), G/B 2.4-3.4 mA (680 R) | 9x |
| Red-off glow | 5.25-3.2 = 2.05 V against red Vf 1.8 V min | firmware sets it as input, 5.5 V tolerant | immune |
| Backlight Q1 AO3400A | 30 V, Vth 1.45 V max | 5 V, 3.3 V drive, 64 mA | wide |
| IR U2 IRM-H638T | Vs 2.7-5.5 V; RC 100 R/4.7 uF | 3.2 V - 0.07 V = 3.13 V min | 0.43 V over min; filter 9.6 mm away (N1) |
| LCD J2 | dual-contact FPC-05FB-12PH20, tail flat, contacts up | yes (README; XUNPU drawing "H2.0" FB) | ok |
| LCD power | VCI 2.4-3.3, IOVCC 1.65-3.3, abs 4.6 | 3.26-3.40 V | N2 |
| Encoder | Alps fig. 5 holes 5 x 1.0 mm, lugs at +-6.25 | 1.2 mm holes, 1.7 x 2.8 slots at +-6.25 | N3 |
| Buttons | TS-1187A 5.1 mm, 4 pads 7.0 x 4.5 | footprint TL_H1.5 | CPL/model fit pass |
| Bulk | C2/C3 10 uF X5R 25 V 0805 at J1 | at 5 V, about -30 % DC bias | ok |

## Layout and DFM

- **DRC and ERC:** KiCad 10.0.6 DRC (zones refilled, schematic parity, all
  severities) and ERC, on a /tmp copy, report 0 violations, 0 unconnected and
  0 parity issues.
- **Stackup and vias:** 2 layers, 1.6 mm, HASL (R5-UF-6). 67 vias, all 0.6/0.3
  and tented. Minimum clearance rule 0.127 mm, hole-to-hole 0.5 mm.
- **GND:** poured on both layers. The F.Cu pour is cut into islands by the long
  UI runs (ENC_A 125 mm, IR_OUT 120 mm, LED_x 92-110 mm). B.Cu is mostly
  solid, with 17 GND vias.
- **Signal routing:** the only fast signals are SCK/MOSI, routed 20-31 mm over
  B.Cu GND. Slow UI lines over a cut plane are acceptable here, apart from
  ESD return (R5-UF-2).
- **Keep-outs:** no parts in the LCD rectangle (KiCad x 15.5-67.3). Traces and
  tented vias under it are fine under 0.25 mm tape.

**Fab package against the PCB:**

| Check | Result |
|---|---|
| `sims/uf/fab_check.py` | PASS: 36 refs in BOM, CPL, netlist, parts.json and PCB; values and LCSC numbers agree; CPL x/y equal the PCB within 0.01 mm; rotations equal the PCB; the zip equals the loose Gerbers byte for byte |
| Fresh `kicad-cli` Gerber and drill export against fab/gerbers | all 10 files identical after stripping header comments (md5 of the bodies) |
| `docs/review-3/sims/layout/cpl_pin1.py` (EasyEDA model pads turned by the CPL rotation onto the board pads) | 36 parts, 0 FAIL; 1 CHECK, U2's silk dot (N9) |

**Assembly:**

- JLC does SW4 (EC11) as THT, by hand.
- Six extended parts are in `prebuy.csv`. Stock at 100 boards: EC11 C361165
  3224 is the thinnest.

## Issue #108's facts (checked again, independently)

- **FFC J3 against ultra-front:** holds (above).
- **TOSLINK U19, PLR237/T8BK (C17223620), against Everlight DPL-0000262/263
  Rev.1 p.5:**
  - Bottom view: pins 3-2-1 left to right with the port up; 1 Vout, 2 GND,
    3 Vcc. Seen from the top with the port up, Vout is on the left.
  - Core U19 at 180 deg with the port to the rear edge: pad 3 (OPT_SPDIF) is
    at x 137.447 (left) and pad 1 (OPT_VCC) at 142.527 (right). The footprint
    numbers its pads the other way round from Everlight, but the nets land on
    the right pins.
  - Rows: posts 5.88 from the port face, NC row 2.61 further, signal row 2.62
    further (datasheet 5.88 / 2.61 / 2.63).
  - Pitches: posts 5.60 apart, NC 5.08, signals 2.54. Holes: posts 1.70 NPTH,
    NC 1.10, signals 0.914 (datasheet 0.80 +-0.05; 0.11 larger is fine for a
    0.5 x 0.25 pin).
  - Fit: the port face is 0.70 mm inside the rear edge. The panel fit is the
    enclosure check's job.
- **USB4085 J1 against GCT drawing rev B, 28/02/23, sheet 2 (component side,
  mouth down):**
  - Rows 1.35 apart (A row away from the mouth), pitch 0.85, A1 left with B12
    under it. Turned 90 deg clockwise onto the board, A1 is at y 175.72 and
    A12 at 181.68. That is not mirrored.
  - Long slots 0.98 / 0.37 from the A / B rows, short slots 3.38 behind them,
    and the edge 1.74 from the short slots (36.327 - 34.587).
  - Slots: 0.6 x 2.1 and 0.6 x 1.4 drills, at +-4.33.
  - Nets: VBUS on A4/A9/B4/B9, GND on A1/A12/B1/B12 and the shell, CC1 A5,
    CC2 B5, D+ A6/B6, D- A7/B7, SBU open. All as GCT sheet 1.
  - Pads are 0.70 against the drawing's 0.65, on purpose (round 1 DFM-6).

  Everything holds.

## Round-2 fixes checked

- **R3-DIGITAL-3 (major, ultra-front): holds.** The firmware drives the panel,
  the expander, the buttons, the LED and IR (above). Its knob direction is
  R5-UF-1, a new defect shared with Plus.
- **R3-DIGITAL-10 (minor): holds.** `hifi_display_reset` re-writes U1's outputs
  and pulses the LCD's reset on every boot. Both INT sources are read each loop.
- **R3-LAYOUT-5 (note): holds for ultra-front.** cpl_pin1 re-run: 36/36 placed.
- **R3-LAYOUT-10 (note): holds for ultra-front.** DRC re-run as above.

## Field issues searched

| Issue | Source | Our board | How |
|---|---|---|---|
| ST7789 with CS tied low needs SPI mode 3, or it never syncs | TFT_eSPI / Adafruit forum threads on 240x240 CS-less modules | immune | DISPLAY_CONFIG `cs=-1,mode=3` |
| ST7789 left mid-command after an MCU crash; the driver sends no SWRESET | R3-DIGITAL-3/10 | immune | hardware reset through U1 P10 each boot |
| RGB LED on 5 V anode glows when a 3.3 V push-pull pin is "high" | common TCA95xx/PCF8574 LED threads | immune | red off = input (5.5 V tolerant); G/B Vf too high to glow at 2.05 V |
| TCA95xx INT stuck low if a port isn't read | TI TCA9535 DS 8.3.3 | immune | both ports of both expanders read each loop, plus a 10 ms poll |
| ESP32 GPIO36/39 spurious low glitch | Espressif ESP32 errata 3.11 | has it, harmless | N5 |
| EC11 contact chatter up to 3 ms | Alps p.282 | immune | 100 us RC, Gray decode cancels bounces (enc_rev.c) |
| Encoder counts the wrong way / every other click | hobby EC11 threads (A/B convention) | **has it** | R5-UF-1 |
| I2C bus held low by a slave after a mid-transaction MCU reset (TCA9535 has no RESET pin) | general I2C; NXP UM10204 3.1.16 | probably immune, unverified | ESP-IDF clears the bus on driver install; same risk as the other 4 slaves; not tested |
| IR receiver false triggers under a PWM-dimmed TFT backlight | Vishay/Everlight app notes | immune | backlight is DC (on/off); IRM-H6 "high immunity against TFT backlight" |
| Metal-actuator tact switches taking ESD through printed caps | consumer-product ESD practice | partial | R5-UF-2 |
| Wrong FFC type (A vs B) at assembly | very common maker issue | possible | R5-UF-5 |

## Tests built (docs/review-5/sims/uf/)

| Path | Checks | Result |
|---|---|---|
| `ffc_orient.py` | J3/J1 same part, pin N = pin N, mouths to the cable edges, pin-1 box x, contact face and cable type | pass |
| `fab_check.py` | BOM/CPL/netlist/parts.json/PCB refs, values, LCSC, CPL x/y/rot, zip against loose Gerbers | pass |
| `tb_sck_asbuilt.cir` + `.bounds.json` (hwde sim_run) | SCK at the ST7789 at 20 MHz: arrival, ring-back against VIH/VIL, over/undershoot | pass (3.25 V high min, 0.05 V low max, 3.77/-0.47 V) |
| `enc_rev.c` (cc against firmware hifi_exp.c) | 18 steps a turn, chatter-proof, direction against Alps CW | **fail** (direction, R5-UF-1) |
| `pcbsum.py` | per-net routed length/width/layers/vias and zones (information only) | n/a |
| re-used `docs/review-3/sims/layout/cpl_pin1.py`, `docs/review-3/sims/digital/i2c_bus.py` | CPL against EasyEDA models; I2C load and addresses | pass / pass |
